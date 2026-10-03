// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("HTTP/1.1 + SSE ... rewritten as
// coroutines"), §4.4 (deadlines are timers, two-phase cancel), §4.5 rule 5 (consumer-stall deadline) and §6.3's
// round-3 gaps 6/7 -- rt/http_wire.hpp (pure) and rt/http.hpp (on the reactor), plain HTTP. The peer is a scripted
// loopback server on a test thread (pal/net.hpp sockets); always built. HTTPS is tests/rt/test_rt_https.cpp. Each
// claim names the mutant that kills it (run 2026-10-03; outcomes in ADR-237 §14).
//
//   H1  GET and POST round trips: status, reason, headers and body come back; the request on the wire is exactly
//       the built one (Host with the non-default port, Content-Length, Connection: close); every await resumes on
//       the block_on() thread (§4.2). Mutant "inline wake" (reactor_await resumes on the reactor thread).
//   H2  Chunked decoding, extensions and trailers, delivered one byte per TCP write. Mutant "raw chunked" (the
//       decoder copies wire bytes through).
//   H3  Caps: head bytes, header count, a declared Content-Length over the cap (refused before reading it), a
//       close-delimited and a chunked body over the cap (stopped DURING the read). Mutant "no until-close cap".
//   H4  Smuggling class, end to end and at the parser: CL + TE, differing duplicate CLs, a CL comma list that
//       disagrees, TE other than exactly chunked, obs-fold, whitespace before a colon, bare LF, non-numeric CL;
//       identical duplicate CLs are accepted. Mutants "no CL+TE check", "no CL conflict check", "no obs-fold check".
//   H5  CR/LF/NUL injection is rejected at build time -- header value, header name, target, method, host -- and a
//       client-managed header (Host / Content-Length / Transfer-Encoding / Connection) is refused; nothing connects.
//       Mutant "no injection check on header values" (the server receives the injected header).
//   H6  Address policy: a rejecting policy -> `address_rejected`, no policy -> `no_policy`; the server never sees a
//       connection. Mutant "policy bypass" (connect with an allow-all policy).
//   H7  No redirect is followed (ADR-011 C10): a 302 comes back as-is, exactly one connection.
//   H8  SSE decoder: generated event streams (multi-line data, comments, id, retry, BOM, LF/CRLF/CR line ends)
//       fed at fuzzed split points equal the generator's events, for 300 seeds; and end to end over chunked HTTP
//       one byte per write. Mutants "CRLF split is two line ends", "data lines not joined", "BOM kept", "all
//       leading spaces stripped".
//   H9  SSE max event size: an oversized event, and a line that never ends, are `event_too_large`; the stream is
//       closed (the server sees it).
//   H10 Deadlines are timers: a server that stalls after the head -> `idle_timeout` within bound; a server that
//       trickles (never idle) -> `deadline_exceeded` from the overall deadline. Mutants "no idle timer", "no overall
//       timer" (a 3 s safety stop then reports `canceled`).
//   H11 A stop mid-body resumes promptly `canceled`, on the home thread, and the server sees the connection close.
//       Mutant "caller stop not linked" (the safety deadline answers instead).
//   H12 Consumer-stall deadline (§4.5 rule 5.3): a consumer that does not call next() within the bound gets
//       `consumer_stalled` (a `resource` error) and the server sees the close; a consumer reading every 40 ms
//       under a 250 ms bound is never stalled; an SSE consumer draining queued events slowly is not stalled,
//       one that stops asking after one event (more queued) is. Mutants "stall timer never armed", "SSE does
//       not re-arm per event".
//   H13 A frame destroyed while its next() is pending: nothing is ever posted to its home, the server sees the
//       close, and the reader reports `canceled` afterwards.
//   H14 One read outstanding: a second next() while one is pending is `busy` and disturbs nothing.
//   H15 Truncation: a body short of its Content-Length, a chunked body with no final chunk, a cut head --
//       `truncated`, with status kept. Mutant "eof always ends cleanly".
//   H16 Interim 1xx responses are skipped; HEAD and 204 have no body whatever Content-Length says.
//   H17 Differential (framing): the existing SseEventFramer's blocks and the new decoder's events agree on every
//       generated single-data-line stream at fuzzed splits (the full field-level differential against the vendor
//       parsers is in test_rt_https.cpp, which links them).
//
// Coroutines are free functions taking their state by pointer, never immediately-invoked lambda coroutines with
// captures (ADR-237 §14); result containers are declared before the reactor/pools that complete into them.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"
#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/http.hpp"
#include "agentengine/rt/http_wire.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/sandbox/incremental_http_body.hpp"
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

constexpr std::uint64_t kLoopback = 0x7F000001;

std::string header_of(HttpHeaders const& h, std::string_view name) {
    for (auto const& [k, v] : h) {
        if (http_wire::equals_ci(k, name)) return v;
    }
    return {};
}

// ---- the scripted loopback server ---------------------------------------------------------------------------

struct Step {
    enum class kind { send, sleep, hold };
    kind        k = kind::send;
    std::string bytes;
    std::size_t piece = 0;  // send: bytes per write (0 = all at once), 1 ms apart
    int         ms    = 0;  // sleep: duration; hold: how long to wait for the client to close
};
Step say(std::string b, std::size_t piece = 0) { return Step{Step::kind::send, std::move(b), piece, 0}; }
Step wait_ms(int ms) { return Step{Step::kind::sleep, {}, 0, ms}; }
Step hold(int ms = 5000) { return Step{Step::kind::hold, {}, 0, ms}; }

// Every accepted connection: read the request (head + Content-Length body), then run the script, then close.
class ScriptServer {
public:
    explicit ScriptServer(std::vector<Step> script) : script_(std::move(script)) {
        auto l = pal::tcp_listen(kLoopback, 0, 64);
        if (l) {
            fd_   = *l;
            port_ = pal::local_port(fd_).value_or(0);
        }
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~ScriptServer() {
        thread_.request_stop();
        thread_.join();
        pal::close_fd(fd_);
    }
    ScriptServer(ScriptServer const&)            = delete;
    ScriptServer& operator=(ScriptServer const&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int           accepted() const { return accepted_.load(); }
    [[nodiscard]] int           saw_close() const { return saw_close_.load(); }  // holds that ended by EOF
    [[nodiscard]] std::string   request(std::size_t i = 0) {
        std::lock_guard lock(m_);
        return i < requests_.size() ? requests_[i] : std::string{};
    }

private:
    bool send_all(pal::fd_t fd, std::string_view s, std::stop_token const& st) {
        std::size_t off = 0;
        auto const  end = std::chrono::steady_clock::now() + 10s;
        while (off < s.size() && !st.stop_requested()) {
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
        return off == s.size();
    }

    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = pal::accept_one(fd_);
            if (!a) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            accepted_.fetch_add(1);
            serve(*a, st);
            pal::close_fd(*a);
        }
    }

    void serve(pal::fd_t fd, std::stop_token const& st) {
        std::string req;
        std::byte   buf[4096];
        auto const  end = std::chrono::steady_clock::now() + 5s;
        std::size_t want = std::string::npos;
        while (!st.stop_requested() && std::chrono::steady_clock::now() < end) {
            auto const he = req.find("\r\n\r\n");
            if (he != std::string::npos && want == std::string::npos) {
                want            = he + 4;
                auto const cl   = req.find("Content-Length: ");
                if (cl != std::string::npos && cl < he) want += std::stoul(req.substr(cl + 16));
            }
            if (want != std::string::npos && req.size() >= want) break;
            auto r = pal::recv_some(fd, buf, sizeof(buf));
            if (r && *r > 0) {
                req.append(reinterpret_cast<char const*>(buf), *r);
            } else if (r || r.error() != pal::would_block()) {
                break;
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        {
            std::lock_guard lock(m_);
            requests_.push_back(req);
        }
        for (Step const& s : script_) {
            if (st.stop_requested()) return;
            switch (s.k) {
                case Step::kind::send:
                    if (s.piece == 0) {
                        if (!send_all(fd, s.bytes, st)) return;
                    } else {
                        for (std::size_t i = 0; i < s.bytes.size(); i += s.piece) {
                            if (!send_all(fd, std::string_view(s.bytes).substr(i, s.piece), st)) return;
                            std::this_thread::sleep_for(1ms);
                        }
                    }
                    break;
                case Step::kind::sleep: std::this_thread::sleep_for(std::chrono::milliseconds(s.ms)); break;
                case Step::kind::hold: {
                    auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.ms);
                    while (!st.stop_requested() && std::chrono::steady_clock::now() < until) {
                        auto r = pal::recv_some(fd, buf, sizeof(buf));
                        if ((r && *r == 0) || (!r && r.error() != pal::would_block())) {
                            saw_close_.fetch_add(1);
                            return;
                        }
                        std::this_thread::sleep_for(1ms);
                    }
                    return;
                }
            }
        }
    }

    std::vector<Step>        script_;
    pal::fd_t                fd_   = pal::invalid_fd;
    std::uint16_t            port_ = 0;
    std::atomic<int>         accepted_{0};
    std::atomic<int>         saw_close_{0};
    std::mutex               m_;
    std::vector<std::string> requests_;
    std::jthread             thread_;  // last: started after everything above exists
};

// ---- a host Resumer with its own thread (as in test_rt_reactor_tcp.cpp) -----------------------------------------

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
    [[nodiscard]] int posted() {
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

// ---- the client side ------------------------------------------------------------------------------------------

bool allow_loopback(pal::IpAddress const& a) { return a == pal::IpAddress::loopback_v4(); }

HttpContext context(pal::Reactor* reactor, OffloadPool* dns, HttpOptions options = {}) {
    HttpContext c;
    c.reactor  = reactor;
    c.dns_pool = dns;
    c.policy   = allow_loopback;
    c.options  = options;
    return c;
}

HttpRequest get(std::uint16_t port, std::string target = "/") {
    HttpRequest r;
    r.host   = "127.0.0.1";
    r.port   = port;
    r.target = std::move(target);
    return r;
}

struct Timed {
    HttpResponse                 r;
    std::vector<std::thread::id> resumed_on;
    std::chrono::milliseconds    took{0};
};

task<Timed> timed_request(HttpContext ctx, HttpRequest req, std::stop_token stop) {
    Timed      t;
    auto const start = std::chrono::steady_clock::now();
    t.r              = co_await http_request(std::move(ctx), std::move(req), std::move(stop));
    t.took           = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    t.resumed_on.push_back(std::this_thread::get_id());
    co_return t;
}

// A request bounded by a safety stop after `limit`, so a mutant that breaks a deadline fails a check, not the run.
Timed bounded_request(HttpContext ctx, HttpRequest req, std::chrono::milliseconds limit = 3s) {
    std::stop_source        src;
    std::mutex              m;
    std::condition_variable cv;
    bool                    finished = false;
    std::thread             safety([&] {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, limit, [&] { return finished; })) src.request_stop();
    });
    Timed t = block_on(timed_request(std::move(ctx), std::move(req), src.get_token()));
    {
        std::lock_guard lock(m);
        finished = true;
    }
    cv.notify_one();
    safety.join();
    return t;
}

struct Streamed {
    HttpStreamResponse           head;
    std::vector<HttpBodyChunk>   chunks;
    std::vector<std::thread::id> resumed_on;
    std::chrono::milliseconds    took{0};
};

task<Streamed> stream_all(HttpContext ctx, HttpRequest req, std::stop_token stop, int gap_ms) {
    Streamed   s;
    auto const start = std::chrono::steady_clock::now();
    s.head           = co_await http_stream(std::move(ctx), std::move(req), stop);
    s.resumed_on.push_back(std::this_thread::get_id());
    if (!s.head.ok()) co_return s;
    for (;;) {
        if (gap_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
        HttpBodyChunk c = co_await s.head.body.next(stop);
        s.resumed_on.push_back(std::this_thread::get_id());
        bool const last = c.status != body_status::data;
        s.chunks.push_back(std::move(c));
        if (last) break;
    }
    s.took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    co_return s;
}

std::string joined(Streamed const& s) {
    std::string out;
    for (auto const& c : s.chunks) out += c.data;
    return out;
}

task<HttpStreamResponse> open_of(HttpContext ctx, HttpRequest req) {
    co_return co_await http_stream(std::move(ctx), std::move(req));
}
task<HttpBodyChunk> next_of(HttpBodyReader* r, std::stop_token stop = {}) { co_return co_await r->next(std::move(stop)); }
task<void> next_then_flag(HttpBodyReader* r, std::atomic<bool>* ran) {
    (void)co_await r->next();
    ran->store(true, std::memory_order_release);
}

struct SseRun {
    HttpStreamResponse    head;
    std::vector<SseEvent> events;
    SseNext               last;
};
task<SseRun> sse_all(HttpContext ctx, HttpRequest req, std::stop_token stop, std::size_t max_event, int gap_ms) {
    SseRun run;
    run.head = co_await http_stream(std::move(ctx), std::move(req), stop);
    if (!run.head.ok()) co_return run;
    SseStream sse = sse_events(std::move(run.head.body), max_event);
    for (;;) {
        SseNext n = co_await sse.next(stop);
        if (n.status != sse_status::event) {
            run.last = std::move(n);
            break;
        }
        run.events.push_back(std::move(n.event));
        if (gap_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
    co_return run;
}

struct SseFirstThenWait {
    SseNext first, second;
};
task<SseFirstThenWait> sse_first_then(HttpContext ctx, HttpRequest req, int wait_ms) {
    SseFirstThenWait   out;
    HttpStreamResponse head = co_await http_stream(std::move(ctx), std::move(req));
    if (!head.ok()) co_return out;
    SseStream sse = sse_events(std::move(head.body));
    out.first     = co_await sse.next();
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));  // the consumer is busy elsewhere
    out.second = co_await sse.next();
    co_return out;
}

std::string chunked(std::vector<std::string> const& parts, std::string trailer = {}) {
    std::string out;
    char        hex[32];
    for (auto const& p : parts) {
        std::snprintf(hex, sizeof(hex), "%zx", p.size());
        out += hex;
        out += "\r\n" + p + "\r\n";
    }
    out += "0\r\n" + trailer + "\r\n";
    return out;
}

// ---- SSE generator (H8 / H17) ---------------------------------------------------------------------------------

struct Generated {
    std::string           wire;
    std::vector<SseEvent> events;
};

// `simple`: only what the existing provider path also handles -- one data line per event, optional `event:`,
// comments, consistent LF or CRLF, no BOM / id / retry, values without leading spaces.
Generated generate(std::mt19937& rng, bool simple) {
    auto        pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    std::string eol  = simple ? (pick(2) == 0 ? "\n" : "\r\n") : std::string();
    bool        prev_cr = false;  // a lone CR followed by an LF would be ONE line end: never generate that
    auto        nl      = [&]() -> std::string {
        if (simple) return eol;
        int const k = prev_cr ? 1 + pick(2) : pick(3);
        prev_cr     = k == 2;
        switch (k) {
            case 0: return "\n";
            case 1: return "\r\n";
            default: return "\r";
        }
    };
    auto word = [&]() {
        static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz{}\":,0123456789 ";
        std::string           w;
        int const             n = 1 + pick(12);
        for (int i = 0; i < n; ++i) w.push_back(kAlphabet[pick(static_cast<int>(sizeof(kAlphabet) - 1))]);
        if (w.front() == ' ') w.front() = 'x';  // values never start with a space here (see the doc comment)
        return w;
    };
    Generated   g;
    std::string id;
    if (!simple && pick(4) == 0) g.wire += "\xEF\xBB\xBF";
    int const n = 1 + pick(8);
    for (int e = 0; e < n; ++e) {
        SseEvent ev;
        if (pick(3) == 0) g.wire += ":" + word() + nl();  // comment
        if (pick(2) == 0) {
            ev.event = word();
            g.wire += "event: " + ev.event + nl();
        }
        int const lines = simple ? 1 : 1 + pick(3);
        for (int l = 0; l < lines; ++l) {
            std::string const d = word();
            g.wire += (pick(2) == 0 ? "data: " : "data:") + d + nl();
            if (l > 0) ev.data += "\n";
            ev.data += d;
        }
        if (!simple && pick(3) == 0) {
            id = word();
            g.wire += "id: " + id + nl();
        }
        if (!simple && pick(4) == 0) {
            std::uint64_t const r = rng() % 100000;
            g.wire += "retry: " + std::to_string(r) + nl();
            ev.retry = r;
        }
        ev.id = id;
        g.wire += nl();
        g.events.push_back(std::move(ev));
    }
    return g;
}

std::vector<std::string_view> split_randomly(std::string_view s, std::mt19937& rng) {
    std::vector<std::string_view> parts;
    std::size_t                   i = 0;
    while (i < s.size()) {
        std::size_t const n = 1 + rng() % 7;
        parts.push_back(s.substr(i, n));
        i += n;
    }
    return parts;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    std::thread([] {
        std::this_thread::sleep_for(280s);
        std::printf("[FAIL] WATCHDOG: a check did not finish within 280 s -- a lost wake-up or a hang\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();  // test-only watchdog; the process ends with main
    pal::ensure_winsock();

    // ---- pure parser claims (no I/O) -----------------------------------------------------------------------
    {
        // H4 at the parser.
        auto parse = [](std::string head, std::string_view method = "GET") {
            http_wire::HeadParser p;
            http_wire::WireError  e = p.feed(head);
            if (!e.ok()) return e;
            if (!p.done()) return http_wire::fail(http_error::truncated, "incomplete", "");
            http_wire::Framing f;
            return http_wire::determine_framing(method, p.head(), HttpLimits{}, &f);
        };
        check(parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n").error ==
                  http_error::ambiguous_framing,
              "H4: Content-Length + Transfer-Encoding is ambiguous_framing");
        check(parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n").error ==
                  http_error::ambiguous_framing,
              "H4: differing duplicate Content-Lengths are ambiguous_framing");
        check(parse("HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\n").error == http_error::ambiguous_framing,
              "H4: a Content-Length list that disagrees is ambiguous_framing");
        check(parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n").ok(),
              "H4: identical duplicate Content-Lengths are accepted (RFC 9110 §8.6)");
        check(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n").error ==
                  http_error::ambiguous_framing,
              "H4: a transfer coding other than exactly chunked is refused");
        check(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, chunked\r\n\r\n").error ==
                  http_error::ambiguous_framing,
              "H4: chunked twice is refused");
        auto const fold = parse("HTTP/1.1 200 OK\r\nX-A: 1\r\n  folded\r\nContent-Length: 0\r\n\r\n");
        check(fold.error == http_error::malformed_response && fold.code == "net.obs_fold_rejected",
              "H4: obsolete line folding is rejected (net.obs_fold_rejected)");
        check(parse("HTTP/1.1 200 OK\r\nContent-Length : 5\r\n\r\n").error == http_error::malformed_response,
              "H4: whitespace between a field name and its colon is rejected");
        check(parse("HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n").error == http_error::malformed_response,
              "H4: a bare LF in the head is rejected");
        check(parse("HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\n").error == http_error::malformed_response,
              "H4: a non-numeric Content-Length is malformed");
        check(parse("HTTP/1.1 200 OK\r\nX: a\x01" "b\r\n\r\n").error == http_error::malformed_response,
              "H4: a control character in a header value is rejected");
        check(parse("HTTX/1.1 200 OK\r\n\r\n").error == http_error::malformed_response &&
                  parse("HTTP/1.1 20 OK\r\n\r\n").error == http_error::malformed_response,
              "H4: a malformed status line is rejected");

        // H3 at the parser: head caps.
        HttpLimits small;
        small.max_header_bytes = 128;
        small.max_header_count = 3;
        http_wire::HeadParser big(small);
        check(big.feed("HTTP/1.1 200 OK\r\nX-Pad: " + std::string(200, 'p')).error == http_error::header_too_large,
              "H3: a head over max_header_bytes is refused before its blank line arrives");
        http_wire::HeadParser many(small);
        check(many.feed("HTTP/1.1 200 OK\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\n\r\n").error ==
                  http_error::header_too_large,
              "H3: a head over max_header_count is refused");

        // H5 at build time.
        auto build = [](HttpHeaders headers, std::string target = "/", std::string method = "GET",
                        std::string host = "example.com") {
            std::string            out;
            http_wire::RequestLine l{method, target, host, 80, 80, &headers, {}};
            return http_wire::build_request(l, &out);
        };
        check(build({{"X-A", "v\r\nX-Evil: 1"}}).code == "net.header_injection_rejected",
              "H5: CRLF in a header value is rejected at build time");
        check(build({{"X-A\r\nX-Evil", "v"}}).error == http_error::invalid_request,
              "H5: CRLF in a header name is rejected");
        check(build({{"X-A", std::string("a\0b", 3)}}).code == "net.header_injection_rejected",
              "H5: NUL in a header value is rejected");
        check(build({{"X-A", "a\x01" "b"}}).code == "net.header_injection_rejected" &&
                  build({{"X-A", "a\x7f"}}).code == "net.header_injection_rejected" &&
                  build({{"X-A", "tab\tok"}}).ok(),
              "H5: other control characters (and DEL) in a header value are rejected; HTAB is allowed");
        check(build({}, "/a b").error == http_error::invalid_request &&
                  build({}, "/a\r\nGET /evil").error == http_error::invalid_request &&
                  build({}, "http://other/").error == http_error::invalid_request,
              "H5: a target with whitespace, CRLF or not origin-form is rejected");
        check(build({}, "/", "GET /x HTTP/1.1\r\n").error == http_error::invalid_request &&
                  build({}, "/", "G ET").error == http_error::invalid_request,
              "H5: a method that is not a token is rejected");
        check(build({}, "/", "GET", "a.com\r\nX: y").error == http_error::invalid_request &&
                  build({}, "/", "GET", "a.com/b").error == http_error::invalid_request,
              "H5: a host with CRLF or a path character is rejected");
        check(build({{"Host", "x"}}).error == http_error::invalid_request &&
                  build({{"content-length", "3"}}).error == http_error::invalid_request &&
                  build({{"Transfer-Encoding", "chunked"}}).error == http_error::invalid_request &&
                  build({{"Connection", "keep-alive"}}).error == http_error::invalid_request,
              "H5: a client-managed header (Host/Content-Length/Transfer-Encoding/Connection) is refused");
        std::string            ok_out;
        HttpHeaders            ok_headers{{"Accept", "text/event-stream"}};
        http_wire::RequestLine v6{"POST", "/p", "::1", 8080, 80, &ok_headers, "abc"};
        check(http_wire::build_request(v6, &ok_out).ok() &&
                  ok_out == "POST /p HTTP/1.1\r\nHost: [::1]:8080\r\nAccept: text/event-stream\r\n"
                            "Content-Length: 3\r\nConnection: close\r\n\r\nabc",
              "H5: a valid request is built exactly (IPv6 host bracketed, non-default port in Host)");

        // H2 / H15 at the decoder: chunked, byte by byte.
        {
            HttpLimits             lim;
            http_wire::BodyDecoder d(http_wire::Framing{http_wire::body_framing::chunked, 0}, lim);
            std::string const      wire = "5;ext=1\r\nhello\r\n1 ; a=b\r\n \r\n0\r\nX-Trailer: t\r\n\r\nGARBAGE";
            std::string            out;
            bool                   ok = true;
            for (char c : wire) ok = ok && d.feed(std::string_view(&c, 1), &out).ok();
            check(ok && out == "hello " && d.complete(),
                  "H2: chunked with extensions and a trailer decodes byte by byte; bytes after the end are ignored");
            http_wire::BodyDecoder bad(http_wire::Framing{http_wire::body_framing::chunked, 0}, lim);
            check(bad.feed("5\r\nhelloX\r\n", &out).error == http_error::malformed_response &&
                      http_wire::BodyDecoder(http_wire::Framing{http_wire::body_framing::chunked, 0}, lim)
                              .feed("zz\r\n", &out)
                              .error == http_error::malformed_response &&
                      http_wire::BodyDecoder(http_wire::Framing{http_wire::body_framing::chunked, 0}, lim)
                              .feed(std::string(2000, '1'), &out)
                              .error == http_error::malformed_response &&
                      http_wire::BodyDecoder(http_wire::Framing{http_wire::body_framing::chunked, 0}, lim)
                              .feed("11111111111111111\r\n", &out)
                              .error == http_error::malformed_response,
                  "H2: malformed chunked framing (missing CRLF, bad hex, endless size line, overflow) is refused");
        }

        // H8: the SSE decoder against generated streams at fuzzed split points.
        {
            int mismatches = 0;
            for (unsigned seed = 1; seed <= 300; ++seed) {
                std::mt19937          rng(seed);
                Generated const       g = generate(rng, /*simple=*/false);
                SseDecoder            d;
                std::vector<SseEvent> got;
                for (std::string_view part : split_randomly(g.wire, rng)) {
                    if (!d.feed(part, &got).ok()) break;
                }
                if (got != g.events) {
                    if (mismatches == 0) std::printf("  first mismatch at seed %u\n", seed);
                    ++mismatches;
                }
            }
            check(mismatches == 0, "H8: 300 generated SSE streams (multi-line data, comments, id, retry, BOM, "
                                   "LF/CRLF/CR) at fuzzed splits decode to the generator's events");
            SseDecoder            d;
            std::vector<SseEvent> got;
            (void)d.feed("data:  two spaces\n\n:only a comment\n\nevent: x\n\ndata\n\nid: 7\nretry: abc\n\n", &got);
            check(got.size() == 2 && got[0].data == " two spaces" && got[1].data.empty() && got[1].event.empty() &&
                      d.last_event_id() == "7" && !d.retry_ms(),
                  "H8: one leading space is removed; a comment-only or event-only block dispatches nothing; a bare "
                  "`data` line is an empty value; id is sticky; a non-numeric retry is ignored");
            SseDecoder            tail;
            std::vector<SseEvent> none;
            (void)tail.feed("data: unterminated", &none);
            check(none.empty() && tail.incomplete(), "H8: an unterminated event is not dispatched (reported incomplete)");

            // H9 at the decoder.
            SseDecoder            capped(64);
            std::vector<SseEvent> sink;
            check(capped.feed("data: " + std::string(100, 'x') + "\n\n", &sink).error == http_error::event_too_large,
                  "H9: an event over max_event_bytes is event_too_large");
            SseDecoder endless(64);
            bool       refused = false;
            for (int i = 0; i < 100 && !refused; ++i) refused = !endless.feed(std::string(8, 'y'), &sink).ok();
            check(refused, "H9: a line that never ends is refused at the cap, not buffered without bound");
        }

        // H17: framing differential against the existing SseEventFramer.
        {
            int mismatches = 0;
            for (unsigned seed = 1; seed <= 300; ++seed) {
                std::mt19937                   rng(seed * 7919u);
                Generated const                g = generate(rng, /*simple=*/true);
                sandbox::SseEventFramer        framer;
                SseDecoder                     d;
                std::vector<std::string>       blocks;
                std::vector<SseEvent>          got;
                for (std::string_view part : split_randomly(g.wire, rng)) {
                    for (auto& b : framer.feed(part)) blocks.push_back(std::move(b));
                    (void)d.feed(part, &got);
                }
                std::vector<std::string> data_blocks;  // blocks that carry a data line, in order
                for (auto const& b : blocks) {
                    if (b.find("data:") != std::string::npos) data_blocks.push_back(b);
                }
                bool same = data_blocks.size() == got.size();
                for (std::size_t i = 0; same && i < got.size(); ++i) {
                    same = data_blocks[i].find(got[i].data) != std::string::npos;
                }
                if (!same) ++mismatches;
            }
            check(mismatches == 0,
                  "H17: the existing SseEventFramer's data blocks and the new decoder's events agree (300 streams)");
        }
    }

    // ---- end to end on the reactor ------------------------------------------------------------------------
    {
        std::thread::id const main_id = std::this_thread::get_id();
        auto                  reactor = pal::make_default_reactor();
        OffloadPool           dns{{.workers = 2}};

        // H1 ---------------------------------------------------------------------------------------------------
        {
            ScriptServer srv({say("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello")});
            HttpRequest  req = get(srv.port(), "/path?x=1");
            req.headers      = {{"X-Probe", "yes"}};
            Timed t          = bounded_request(context(reactor.get(), &dns), req);
            check(t.r.ok() && t.r.status == 200 && t.r.reason == "OK" && t.r.body == "hello" &&
                      header_of(t.r.headers, "content-type") == "text/plain",
                  "H1: GET round trip -- status, reason, headers and body");
            std::string const expect = "GET /path?x=1 HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) +
                                       "\r\nX-Probe: yes\r\nConnection: close\r\n\r\n";
            check(srv.request() == expect, "H1: the request on the wire is exactly the built one");
            check(t.resumed_on.size() == 1 && t.resumed_on[0] == main_id,
                  "H1: the request resumed on the block_on() thread, its home");

            ScriptServer post_srv({say("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n")});
            HttpRequest  post = get(post_srv.port(), "/items");
            post.method       = "POST";
            post.body         = R"({"a":1})";
            Timed p           = bounded_request(context(reactor.get(), &dns), post);
            check(p.r.ok() && p.r.status == 201 && p.r.body.empty() &&
                      post_srv.request().find("Content-Length: 7\r\nConnection: close\r\n\r\n{\"a\":1}") !=
                          std::string::npos,
                  "H1: POST sends Content-Length and the body; an empty 201 body is fine");

            ScriptServer s2({say("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc")});
            Streamed st = block_on(stream_all(context(reactor.get(), &dns), get(s2.port()), {}, 0));
            bool     home = !st.resumed_on.empty();
            for (auto id : st.resumed_on) home = home && id == main_id;
            check(st.head.ok() && joined(st) == "abc" && st.chunks.back().status == body_status::end && home,
                  "H1: streamed head + body; every await resumed on the block_on() thread");
        }

        // H2 ---------------------------------------------------------------------------------------------------
        {
            std::string const body = chunked({"Hello, ", "chunked ", std::string(300, 'z'), "!"}, "X-T: 1\r\n");
            ScriptServer      srv({say("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"), say(body, 1)});
            Timed             t = bounded_request(context(reactor.get(), &dns), get(srv.port()), 20s);
            check(t.r.ok() && t.r.body == "Hello, chunked " + std::string(300, 'z') + "!",
                  "H2: chunked body (with a trailer) delivered one byte per TCP write decodes exactly");
        }

        // H3 ---------------------------------------------------------------------------------------------------
        {
            HttpOptions o;
            o.limits.max_body_bytes = 1000;
            ScriptServer declared({say("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n"), hold(2000)});
            Timed        d = bounded_request(context(reactor.get(), &dns, o), get(declared.port()));
            check(d.r.error == http_error::body_too_large && d.r.code == "net.byte_cap_exceeded" &&
                      is_resource_error(d.r.error) && d.r.status == 200,
                  "H3: a declared Content-Length over the cap is refused before its body is read (resource)");

            ScriptServer until({say("HTTP/1.1 200 OK\r\n\r\n"), say(std::string(5000, 'u')), hold(3000)});
            Timed        u = bounded_request(context(reactor.get(), &dns, o), get(until.port()));
            check(u.r.error == http_error::body_too_large && u.r.body.size() <= 1000,
                  "H3: a close-delimited body over the cap is stopped during the read, never buffered past it");
            check(wait_until([&] { return until.saw_close() == 1; }, 3s), "H3: ... and the connection is closed");

            ScriptServer ch({say("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" +
                                  chunked({std::string(600, 'a'), std::string(600, 'b')})),
                             hold(2000)});
            Timed        c = bounded_request(context(reactor.get(), &dns, o), get(ch.port()));
            check(c.r.error == http_error::body_too_large, "H3: a chunked body over the cap is body_too_large");

            HttpOptions hdr;
            hdr.limits.max_header_bytes = 256;
            ScriptServer longhead({say("HTTP/1.1 200 OK\r\nX-Pad: " + std::string(4000, 'h') + "\r\n\r\n"), hold(2000)});
            Timed        h = bounded_request(context(reactor.get(), &dns, hdr), get(longhead.port()));
            check(h.r.error == http_error::header_too_large, "H3: a head over max_header_bytes is header_too_large");

            HttpOptions big;
            big.limits.max_body_bytes = std::uint64_t{1} << 40;
            check(big.limits.effective_body_cap() == HttpLimits::kHardBodyCeilingBytes,
                  "H3: max_body_bytes can only narrow the 16 MiB hard ceiling, never widen it");
        }

        // H4 end to end ------------------------------------------------------------------------------------------
        {
            ScriptServer both({say("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n"
                                    "0\r\n\r\nGET /smuggled HTTP/1.1\r\n\r\n"),
                               hold(2000)});
            Timed        t = bounded_request(context(reactor.get(), &dns), get(both.port()));
            check(t.r.error == http_error::ambiguous_framing && t.r.code == "net.framing_conflict" && t.r.body.empty(),
                  "H4: a response with CL + TE is refused end to end, no body delivered");
            ScriptServer dup({say("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 30\r\n\r\nabc"), hold(2000)});
            Timed        d = bounded_request(context(reactor.get(), &dns), get(dup.port()));
            check(d.r.error == http_error::ambiguous_framing && d.r.code == "net.content_length_conflict",
                  "H4: differing duplicate Content-Lengths are refused end to end");
            ScriptServer fold({say("HTTP/1.1 200 OK\r\nX-A: 1\r\n\tcontinued\r\nContent-Length: 0\r\n\r\n"), hold(2000)});
            Timed        f = bounded_request(context(reactor.get(), &dns), get(fold.port()));
            check(f.r.error == http_error::malformed_response && f.r.code == "net.obs_fold_rejected",
                  "H4: obs-fold is refused end to end");
        }

        // H5 end to end: nothing connects --------------------------------------------------------------------------
        {
            ScriptServer srv({say("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")});
            HttpRequest  evil = get(srv.port());
            evil.headers      = {{"X-A", "v\r\nX-Injected: 1"}};
            Timed t           = bounded_request(context(reactor.get(), &dns), evil);
            std::this_thread::sleep_for(50ms);
            check(t.r.error == http_error::invalid_request && t.r.code == "net.header_injection_rejected" &&
                      srv.accepted() == 0,
                  "H5: a CRLF-injected header is refused before any connection (the server saw none)");
        }

        // H6 ---------------------------------------------------------------------------------------------------
        {
            ScriptServer srv({say("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")});
            HttpContext  deny = context(reactor.get(), &dns);
            deny.policy       = [](pal::IpAddress const&) { return false; };
            Timed r           = bounded_request(deny, get(srv.port()));
            HttpContext none  = context(reactor.get(), &dns);
            none.policy       = nullptr;
            Timed n           = bounded_request(none, get(srv.port()));
            std::this_thread::sleep_for(50ms);
            check(r.r.error == http_error::address_rejected && r.r.code == "net.address_blocked",
                  "H6: a policy rejecting every address is address_rejected");
            check(n.r.error == http_error::no_policy, "H6: no policy is no_policy (fail closed, I2)");
            check(srv.accepted() == 0, "H6: the server never saw a connection");
            Timed ok = bounded_request(context(reactor.get(), &dns), get(srv.port()));
            check(ok.r.ok() && srv.accepted() == 1, "H6: positive control -- an accepting policy connects");
        }

        // H7 ---------------------------------------------------------------------------------------------------
        {
            ScriptServer srv({say("HTTP/1.1 302 Found\r\nLocation: http://169.254.169.254/latest/meta-data\r\n"
                                   "Content-Length: 0\r\n\r\n")});
            Timed        t = bounded_request(context(reactor.get(), &dns), get(srv.port()));
            std::this_thread::sleep_for(50ms);
            check(t.r.ok() && t.r.status == 302 && header_of(t.r.headers, "location").starts_with("http://169.254") &&
                      srv.accepted() == 1,
                  "H7: a 302 is returned as received; no redirect is followed (one connection)");
        }

        // H8 end to end --------------------------------------------------------------------------------------------
        {
            std::string const sse = "\xEF\xBB\xBF: hi\r\nevent: delta\r\ndata: {\"a\":1}\r\ndata: line2\r\nid: 9\r\n\r\n"
                                    "data: second\n\nretry: 1500\ndata: third\r\r";
            ScriptServer      srv({say("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                        "Transfer-Encoding: chunked\r\n\r\n"),
                                   say(chunked({sse.substr(0, 20), sse.substr(20, 33), sse.substr(53)}), 1)});
            SseRun r = block_on(sse_all(context(reactor.get(), &dns), get(srv.port()), {}, 1024, 0));
            check(r.head.ok() && r.events.size() == 3 && r.events[0].event == "delta" &&
                      r.events[0].data == "{\"a\":1}\nline2" && r.events[0].id == "9" && r.events[1].data == "second" &&
                      r.events[1].id == "9" && r.events[2].data == "third" && r.events[2].retry == 1500u &&
                      r.last.status == sse_status::end && !r.last.incomplete_dropped,
                  "H8: SSE over chunked HTTP, one byte per write: BOM, comment, event, multi-line data, id, retry, "
                  "LF/CRLF/CR, then end");
        }

        // H9 end to end --------------------------------------------------------------------------------------------
        {
            ScriptServer srv({say("HTTP/1.1 200 OK\r\n\r\ndata: " + std::string(5000, 'x')), hold(3000)});
            SseRun       r = block_on(sse_all(context(reactor.get(), &dns), get(srv.port()), {}, 1024, 0));
            check(r.last.status == sse_status::error && r.last.error == http_error::event_too_large &&
                      is_resource_error(r.last.error),
                  "H9: an SSE event over the cap fails the stream (event_too_large, a resource error)");
            check(wait_until([&] { return srv.saw_close() == 1; }, 3s), "H9: ... and the connection is closed");
        }

        // H10 ----------------------------------------------------------------------------------------------------
        {
            HttpOptions o;
            o.idle_timeout = 200ms;
            ScriptServer stall({say("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc"), hold(5000)});
            Timed        t = bounded_request(context(reactor.get(), &dns, o), get(stall.port()));
            check(t.r.error == http_error::idle_timeout && t.r.code == "net.idle_timeout" && t.r.status == 200 &&
                      t.took < 2s,
                  "H10: a server silent mid-body ends idle_timeout within bound (took " +
                      std::to_string(t.took.count()) + " ms)");
            check(wait_until([&] { return stall.saw_close() == 1; }, 3s), "H10: ... and the connection is closed");

            HttpOptions total;
            total.total_timeout = 300ms;
            total.idle_timeout  = 2s;
            std::vector<Step> trickle{say("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n")};
            for (int i = 0; i < 60; ++i) {
                trickle.push_back(say("."));
                trickle.push_back(wait_ms(50));
            }
            ScriptServer slow(trickle);
            Timed        d = bounded_request(context(reactor.get(), &dns, total), get(slow.port()));
            check(d.r.error == http_error::deadline_exceeded && d.took < 2s,
                  "H10: a trickling server (never idle) ends deadline_exceeded from the overall deadline (took " +
                      std::to_string(d.took.count()) + " ms)");
        }

        // H11 ----------------------------------------------------------------------------------------------------
        {
            HttpOptions o;
            o.total_timeout = 3s;  // the safety answer a broken stop would get instead
            ScriptServer srv({say("HTTP/1.1 200 OK\r\n\r\npartial"), hold(5000)});
            std::stop_source src;
            std::thread      stopper([&] {
                std::this_thread::sleep_for(150ms);
                src.request_stop();
            });
            auto const start = std::chrono::steady_clock::now();
            Streamed   s     = block_on(stream_all(context(reactor.get(), &dns, o), get(srv.port()), src.get_token(), 0));
            auto const took  = std::chrono::steady_clock::now() - start;
            stopper.join();
            bool home = !s.resumed_on.empty();
            for (auto id : s.resumed_on) home = home && id == main_id;
            std::string diag = " [head " + std::to_string(static_cast<int>(s.head.error)) + ", chunks";
            for (auto const& c : s.chunks) diag += " " + std::to_string(static_cast<int>(c.status)) + "/" +
                                                   std::to_string(static_cast<int>(c.error));
            diag += "]";
            check(!s.chunks.empty() && s.chunks.back().status == body_status::error &&
                      s.chunks.back().error == http_error::canceled && took < 1s && home,
                  "H11: a stop mid-body resumes promptly `canceled`, on the home thread (took " +
                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms)" +
                      diag);
            check(wait_until([&] { return srv.saw_close() == 1; }, 3s), "H11: ... and the server sees the close");
        }

        // H12 ----------------------------------------------------------------------------------------------------
        {
            HttpOptions o;
            o.consumer_stall_timeout = 150ms;
            ScriptServer srv({say("HTTP/1.1 200 OK\r\n\r\nfirst"), hold(5000)});
            HttpStreamResponse h = block_on(open_of(context(reactor.get(), &dns, o), get(srv.port())));
            bool const         closed = wait_until([&] { return srv.saw_close() == 1; }, 2s);  // no next() called
            HttpBodyChunk      c = block_on(next_of(&h.body));
            check(closed, "H12: a consumer that never asks for the body: the connection is closed at the stall bound");
            check(c.status == body_status::error && c.error == http_error::consumer_stalled &&
                      c.code == "net.consumer_stalled" && is_resource_error(c.error),
                  "H12: ... and next() reports consumer_stalled (a resource error)");
            HttpBodyChunk again = block_on(next_of(&h.body));
            check(again.error == http_error::consumer_stalled, "H12: consumer_stalled is sticky");

            HttpOptions active;
            active.consumer_stall_timeout = 250ms;
            std::vector<Step> steady{say("HTTP/1.1 200 OK\r\n\r\n")};
            for (int i = 0; i < 10; ++i) {
                steady.push_back(say("x"));
                steady.push_back(wait_ms(30));
            }
            ScriptServer s2(steady);
            Streamed     s = block_on(stream_all(context(reactor.get(), &dns, active), get(s2.port()), {}, 40));
            check(joined(s) == std::string(10, 'x') && s.chunks.back().status == body_status::end,
                  "H12: positive control -- a consumer reading every 40 ms under a 250 ms bound is never stalled");

            std::string many;
            for (int i = 0; i < 6; ++i) many += "data: e" + std::to_string(i) + "\n\n";
            ScriptServer s3({say("HTTP/1.1 200 OK\r\n\r\n" + many)});
            SseRun       r = block_on(sse_all(context(reactor.get(), &dns, active), get(s3.port()), {}, 1024, 100));
            check(r.events.size() == 6 && r.last.status == sse_status::end,
                  "H12: an SSE consumer draining queued events 100 ms apart (600 ms total) is not stalled");

            HttpOptions  quick;
            quick.consumer_stall_timeout = 150ms;
            ScriptServer s4({say("HTTP/1.1 200 OK\r\n\r\n" + many), hold(5000)});
            SseFirstThenWait w = block_on(sse_first_then(context(reactor.get(), &dns, quick), get(s4.port()), 400));
            check(w.first.status == sse_status::event && w.second.status == sse_status::error &&
                      w.second.error == http_error::consumer_stalled,
                  "H12: an SSE consumer that stops asking after one event is consumer_stalled (events still queued)");
            check(wait_until([&] { return s4.saw_close() == 1; }, 3s), "H12: ... and the connection is closed");
        }

        // H13 ----------------------------------------------------------------------------------------------------
        {
            ScriptServer       srv({say("HTTP/1.1 200 OK\r\n\r\nhead-only"), wait_ms(300), say("late data"), hold(5000)});
            HttpStreamResponse h = block_on(open_of(context(reactor.get(), &dns), get(srv.port())));
            HttpBodyChunk      first = block_on(next_of(&h.body));
            auto               resumer = std::make_shared<ThreadResumer>();
            {
                std::atomic<bool> ran{false};
                auto              t = next_then_flag(&h.body, &ran);
                ScopedResumer     scope(resumer);
                t.resume();
                std::this_thread::sleep_for(30ms);  // the read is pending
            }  // frame destroyed mid-read
            check(first.data == "head-only", "H13: (setup) the first chunk arrived");
            check(wait_until([&] { return srv.saw_close() == 1; }, 3s),
                  "H13: destroying the frame mid-read closed the connection");
            std::this_thread::sleep_for(400ms);  // past the server's late data
            check(resumer->posted() == 0, "H13: nothing was ever posted for the destroyed frame");
            HttpBodyChunk after = block_on(next_of(&h.body));
            check(after.status == body_status::error && after.error == http_error::canceled,
                  "H13: the reader reports `canceled` afterwards");
            resumer->stop();
        }

        // H14 ----------------------------------------------------------------------------------------------------
        {
            ScriptServer       srv({say("HTTP/1.1 200 OK\r\n\r\n"), wait_ms(300), say("payload")});
            HttpStreamResponse h       = block_on(open_of(context(reactor.get(), &dns), get(srv.port())));
            auto               resumer = std::make_shared<ThreadResumer>();
            std::atomic<bool>  ran{false};
            auto               t = next_then_flag(&h.body, &ran);
            {
                ScopedResumer scope(resumer);
                t.resume();
            }
            std::this_thread::sleep_for(30ms);
            HttpBodyChunk second = block_on(next_of(&h.body));
            check(second.status == body_status::error && second.error == http_error::busy,
                  "H14: a second next() while one is pending is `busy`");
            check(wait_until([&] { return ran.load(); }, 3s), "H14: ... and the first read completes undisturbed");
            resumer->stop();
        }

        // H15 ----------------------------------------------------------------------------------------------------
        {
            ScriptServer short_cl({say("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly-ten!!")});
            Timed        a = bounded_request(context(reactor.get(), &dns), get(short_cl.port()));
            check(a.r.error == http_error::truncated && a.r.code == "net.stream_truncated" && a.r.status == 200 &&
                      a.r.body == "only-ten!!",
                  "H15: a body short of its Content-Length is truncated (status and partial body kept)");
            ScriptServer no_final({say("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n")});
            Timed        b = bounded_request(context(reactor.get(), &dns), get(no_final.port()));
            check(b.r.error == http_error::truncated, "H15: a chunked body with no final chunk is truncated");
            ScriptServer cut({say("HTTP/1.1 200 OK\r\nContent-Le")});
            Timed        c = bounded_request(context(reactor.get(), &dns), get(cut.port()));
            check(c.r.error == http_error::truncated && c.r.code == "net.protocol_error",
                  "H15: a cut head is truncated (net.protocol_error)");
            ScriptServer until({say("HTTP/1.1 200 OK\r\n\r\nto the end")});
            Timed        d = bounded_request(context(reactor.get(), &dns), get(until.port()));
            check(d.r.ok() && d.r.body == "to the end", "H15: positive control -- a close-delimited body ends at close");
        }

        // H16 ----------------------------------------------------------------------------------------------------
        {
            ScriptServer cont({say("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")});
            Timed        a = bounded_request(context(reactor.get(), &dns), get(cont.port()));
            check(a.r.ok() && a.r.status == 200 && a.r.body == "ok", "H16: interim 1xx responses are skipped");
            ScriptServer head({say("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n"), hold(3000)});
            HttpRequest  hr = get(head.port());
            hr.method       = "HEAD";
            Timed h         = bounded_request(context(reactor.get(), &dns), hr);
            check(h.r.ok() && h.r.body.empty() && h.took < 2s, "H16: a HEAD response has no body whatever CL says");
            ScriptServer nc({say("HTTP/1.1 204 No Content\r\n\r\n"), hold(3000)});
            Timed        n = bounded_request(context(reactor.get(), &dns), get(nc.port()));
            check(n.r.ok() && n.r.status == 204 && n.took < 2s, "H16: a 204 has no body and does not wait for close");
        }

        // https without a connector is refused before connecting (the TLS path itself is test_rt_https.cpp).
        {
            ScriptServer srv({say("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")});
            HttpRequest  r = get(srv.port());
            r.scheme       = http_scheme::https;
            Timed t        = bounded_request(context(reactor.get(), &dns), r);
            std::this_thread::sleep_for(50ms);
            check(t.r.error == http_error::tls_unavailable && srv.accepted() == 0,
                  "https without a TLS connector is tls_unavailable; nothing connects");
        }
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
