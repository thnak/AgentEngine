// Proves decisions/archive/ADR-241-egress-proxy-async-path-on-rt-http.md section 8.2 / 8.3 and the gates G7 (host-Runtime and
// when_all cancellation), G13 (the egress I/O context and admission, rows e, g, h, h3, i, j, k) of section 11.
//
// Loopback fake servers and injected resolvers only; every call is bounded by a stop token or a short deadline, and the
// process is memory-capped (CLAUDE.md "Machine safety"). The first-use rows (G13 b, d, f) need a fresh process and live in
// test_net_egress_first_use.cpp.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "../support/memory_cap.hpp"
#include "agentengine/pal/net.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/scope.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/sandbox/detail/net_egress_impl.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/sandbox/testing/net_egress_proxy_testing.hpp"
#include "agentengine/trust/principal.hpp"

namespace {

using agentengine::cap::NetOut;
using agentengine::failure_class;
using agentengine::result;
using agentengine::sandbox::AdmissionKey;
using agentengine::sandbox::EgressCallControl;
using agentengine::sandbox::NetEgressRequest;
using agentengine::sandbox::NetEgressResponse;
using agentengine::sandbox::TestEgressProxy;
namespace egress = agentengine::sandbox::egress_detail;
using clk = std::chrono::steady_clock;
using std::chrono::milliseconds;

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

constexpr std::uint32_t kLoopbackHostOrder = (127u << 24) | 1u;

bool write_all(agentengine::pal::fd_t fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto r = agentengine::pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent), data.size() - sent);
        if (!r) {
            if (r.error() == agentengine::pal::would_block()) {
                std::this_thread::sleep_for(milliseconds(2));
                continue;
            }
            return false;
        }
        sent += *r;
    }
    return true;
}

// Answers every connection with a 200 "hello" (reads the head first so closing never RSTs the client).
class FastServer {
public:
    FastServer() {
        auto l = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        if (!l) return;
        fd_     = *l;
        port_   = *agentengine::pal::local_port(fd_);
        thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested()) {
                auto a = agentengine::pal::accept_one(fd_);
                if (!a) {
                    std::this_thread::sleep_for(milliseconds(2));
                    continue;
                }
                std::string buf;
                std::array<std::byte, 4096> chunk{};
                for (int i = 0; i < 400 && buf.find("\r\n\r\n") == std::string::npos; ++i) {
                    auto r = agentengine::pal::recv_some(*a, chunk.data(), chunk.size());
                    if (!r) {
                        if (r.error() == agentengine::pal::would_block()) {
                            std::this_thread::sleep_for(milliseconds(2));
                            continue;
                        }
                        break;
                    }
                    if (*r == 0) break;
                    buf.append(reinterpret_cast<char const*>(chunk.data()), *r);
                }
                write_all(*a, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
                agentengine::pal::close_fd(*a);
            }
        });
    }
    ~FastServer() {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
        agentengine::pal::close_fd(fd_);
    }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    agentengine::pal::fd_t fd_{};
    std::uint16_t          port_ = 0;
    std::jthread           thread_;
};

// Listens and NEVER accepts: the kernel completes the handshake into the backlog, so a client connects and then waits
// for a response that never comes -- a call that holds its admission slot until it is stopped.
class SilentListener {
public:
    SilentListener() {
        auto l = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        if (!l) return;
        fd_   = *l;
        port_ = *agentengine::pal::local_port(fd_);
    }
    ~SilentListener() { agentengine::pal::close_fd(fd_); }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    agentengine::pal::fd_t fd_{};
    std::uint16_t          port_ = 0;
};

bool permit_all(agentengine::pal::IpAddress const&) noexcept { return true; }

struct LoopbackDns {
    agentengine::pal::ResolveOutcome operator()(std::string const&) const {
        agentengine::pal::ResolveOutcome out;
        out.addresses.push_back(agentengine::pal::IpAddress::v4(127, 0, 0, 1));
        return out;
    }
};

NetOut grant_to(std::uint16_t port) {
    NetOut n{{}, agentengine::cap::Limit::unbounded(), {}};
    n.host_allowlist = {"test.invalid:" + std::to_string(port) + ":http"};
    return n;
}

NetEgressRequest get_root() { return NetEgressRequest{"GET", "/", {}, ""}; }

AdmissionKey key(std::string tenant, std::string principal, std::string run) {
    return AdmissionKey{std::move(tenant), std::move(principal), std::move(run)};
}

void configure(TestEgressProxy& p) {
    p.host_resolver  = LoopbackDns{};
    p.address_policy = permit_all;
    p.total_timeout  = std::chrono::seconds(10);
}

// One synchronous call, timed.
struct Timed {
    result<NetEgressResponse> r;
    long long                 ms = 0;
};
Timed timed_call(AdmissionKey k, std::uint16_t port, std::stop_token stop = {}) {
    TestEgressProxy p;
    configure(p);
    EgressCallControl c;
    c.stop      = std::move(stop);
    c.admission = std::move(k);
    auto const t0 = clk::now();
    auto r        = p.fetch_blocking(get_root(), grant_to(port), c);
    return Timed{std::move(r), std::chrono::duration_cast<milliseconds>(clk::now() - t0).count()};
}

// N calls that hold their slots (against the silent listener) until released.
class HeldCalls {
public:
    void launch(int n, AdmissionKey const& k, std::uint16_t port) {
        for (int i = 0; i < n; ++i) {
            threads_.emplace_back([this, k, port] {
                auto t = timed_call(k, port, stop_.get_token());
                (void)t;
            });
        }
    }
    void release() {
        stop_.request_stop();
        threads_.clear();  // jthread joins
    }
    ~HeldCalls() { release(); }

private:
    std::stop_source        stop_;
    std::vector<std::jthread> threads_;
};

bool wait_for_global(std::uint32_t n, milliseconds timeout = std::chrono::seconds(15)) {
    auto const end = clk::now() + timeout;
    while (clk::now() < end) {
        if (egress::egress_admission_snapshot().global == n) return true;
        std::this_thread::sleep_for(milliseconds(5));
    }
    return egress::egress_admission_snapshot().global == n;
}

void expect_busy(char const* row, Timed const& t) {
    check(!t.r.has_value() && t.r.error().code == "net.egress_busy" && t.r.error().klass == failure_class::resource,
          std::string(row) + ": net.egress_busy / resource (was " + (t.r ? "success" : t.r.error().code) + ")");
    check(t.ms >= 900 && t.ms < 3000, std::string(row) + ": waited ~1 s, not instantly (" + std::to_string(t.ms) + " ms)");
    if (!t.r) {
        check(t.r.error().message.find("retry") != std::string::npos, std::string(row) + ": the message carries a retry hint");
    }
}

void expect_ok(char const* row, Timed const& t) {
    check(t.r.has_value() && t.r->status == 200, std::string(row) + ": succeeds");
}

struct LimitsRestore {
    std::uint32_t run, pr, global, ur, up;
    LimitsRestore() {
        auto& l = agentengine::sandbox::egress_limits();
        run     = l.per_run.load();
        pr      = l.per_principal.load();
        global  = l.global.load();
        ur      = l.unattributed_run.load();
        up      = l.unattributed_principal.load();
    }
    ~LimitsRestore() {
        auto& l = agentengine::sandbox::egress_limits();
        l.per_run.store(run);
        l.per_principal.store(pr);
        l.global.store(global);
        l.unattributed_run.store(ur);
        l.unattributed_principal.store(up);
    }
};

// ---- G13 h: the run level -------------------------------------------------------------------------------------
void g13_h_run_level(SilentListener& silent, FastServer& fast) {
    HeldCalls a;
    a.launch(8, key("t", "p", "A"), silent.port());
    check(wait_for_global(8), "G13h: eight calls of run A are in flight");
    expect_busy("G13h the 9th call of run A", timed_call(key("t", "p", "A"), fast.port()));
    // The same principal's OTHER run still fetches (a saturated run cannot starve its sibling).
    auto const b = timed_call(key("t", "p", "B"), fast.port());
    expect_ok("G13h run B of the same principal", b);
    check(b.ms < 800, "G13h run B is admitted immediately (" + std::to_string(b.ms) + " ms)");
    auto const snap = egress::egress_admission_snapshot();
    check(snap.runs == 1 && snap.principals == 1, "G13h one run entry and one principal entry while A is saturated");
    a.release();
    check(wait_for_global(0), "G13h every slot is released");
    auto const after = egress::egress_admission_snapshot();
    check(after.runs == 0 && after.principals == 0, "G13j the maps are empty again after every slot is released (erase-when-zero)");
}

// ---- G13 i: the principal level, keyed on root_id() -------------------------------------------------------------
void g13_i_principal_level(SilentListener& silent, FastServer& fast) {
    using agentengine::EffectContext;
    agentengine::Principal root;
    root.id        = "alice";
    root.tenant_id = "t";
    // Five root runs, each fetching through a DIFFERENT spawned child principal of the same root (agent.spawn): every
    // child's key charges the ROOT principal's bucket, so the five runs are held to 32 in total.
    std::vector<AdmissionKey> keys;
    for (int run = 0; run < 5; ++run) {
        auto child = agentengine::derive_on_behalf_of(root, "alice-child-" + std::to_string(run));
        EffectContext ctx;
        ctx.principal   = *child;
        ctx.root_run_id = "run-" + std::to_string(run);
        ctx.run_id      = "child-run-" + std::to_string(run);  // the child's OWN run id must not matter
        keys.push_back(agentengine::sandbox::egress_call_control(ctx).admission);
    }
    check(keys[0].principal == "alice" && keys[3].principal == "alice",
          "G13i the principal level keys on root_id() (a spawned child shares its root's bucket)");
    check(keys[0].root_run == "run-0", "G13i the run level keys on root_run_id, not the child's run id");

    std::vector<std::unique_ptr<HeldCalls>> groups;
    for (int run = 0; run < 4; ++run) {
        groups.push_back(std::make_unique<HeldCalls>());
        groups.back()->launch(8, keys[run], silent.port());
    }
    check(wait_for_global(32), "G13i four runs x 8 = 32 calls of one principal are in flight");
    expect_busy("G13i the 33rd call of the principal (a fifth run, via a child)", timed_call(keys[4], fast.port()));
    // Another principal still succeeds.
    expect_ok("G13i another principal", timed_call(key("t", "bob", "run-0"), fast.port()));
    for (auto& g : groups) g->release();
    check(wait_for_global(0), "G13i released");
}

// ---- G13 k / R5-1: an empty identity charges the UNATTRIBUTED bucket, it never skips a level ---------------------
void g13_k_unattributed(SilentListener& silent, FastServer& fast) {
    {  // red-team r5 m2: the shipped defaults. The unattributed PRINCIPAL bucket is the per-principal value (a host that
       // never sets principals is not throttled below one real principal); the unattributed RUN bucket stays 8.
        auto const& l = agentengine::sandbox::egress_limits();
        check(l.unattributed_principal.load() == 32 && l.per_principal.load() == 32,
              "G13k default unattributed_principal == per_principal == 32");
        check(l.unattributed_run.load() == 8, "G13k default unattributed_run == 8");
    }
    {  // an empty run identity (and empty principal): the unattributed buckets of both levels
        HeldCalls h;
        h.launch(8, key("", "", ""), silent.port());
        check(wait_for_global(8), "G13k eight empty-identity calls are in flight");
        expect_busy("G13k the 9th empty-identity call", timed_call(key("", "", ""), fast.port()));
        // An attributed principal still gets through while the unattributed bucket is full.
        expect_ok("G13k an attributed principal", timed_call(key("t", "alice", "run-1"), fast.port()));
        h.release();
        check(wait_for_global(0), "G13k released");
    }
    {  // an empty PRINCIPAL with distinct runs: the unattributed PRINCIPAL bucket holds them to its cap (set to 8 here so
       // the row is cheap; the shipped default is asserted above) across runs
        LimitsRestore restore;
        agentengine::sandbox::egress_limits().unattributed_principal.store(8);
        HeldCalls h;
        h.launch(4, key("t", "", "R1"), silent.port());
        h.launch(4, key("t", "", "R2"), silent.port());
        check(wait_for_global(8), "G13k 8 empty-principal calls across two runs are in flight");
        expect_busy("G13k the 9th empty-principal call (a fresh run)", timed_call(key("t", "", "R3"), fast.port()));
        h.release();
        check(wait_for_global(0), "G13k released (principal level)");
    }
    {  // an empty RUN with an attributed principal: the unattributed RUN bucket, shared by all principals
        HeldCalls h;
        h.launch(4, key("t", "p1", ""), silent.port());
        h.launch(4, key("t", "p2", ""), silent.port());
        check(wait_for_global(8), "G13k 8 empty-run calls across two principals are in flight");
        expect_busy("G13k the 9th empty-run call", timed_call(key("t", "p3", ""), fast.port()));
        h.release();
        check(wait_for_global(0), "G13k released (run level)");
    }
}

// ---- G13 e: the global cap waits, then fails; a slot freed inside the wait is taken ---------------------------------
void g13_e_global_cap(SilentListener& silent, FastServer& fast) {
    LimitsRestore restore;
    auto& l = agentengine::sandbox::egress_limits();
    l.per_run.store(1000);
    l.per_principal.store(1000);
    l.global.store(4);
    std::vector<std::unique_ptr<HeldCalls>> holders;
    for (int i = 0; i < 4; ++i) {
        holders.push_back(std::make_unique<HeldCalls>());
        holders.back()->launch(1, key("t", "p", "G" + std::to_string(i)), silent.port());
    }
    check(wait_for_global(4), "G13e four calls fill the global cap");
    expect_busy("G13e the 5th call", timed_call(key("t", "p", "G5"), fast.port()));
    // A slot that frees inside the wait window is taken by the waiter.
    Timed waited;
    std::jthread waiter([&] { waited = timed_call(key("t", "p", "G6"), fast.port()); });
    std::this_thread::sleep_for(milliseconds(300));
    holders[0]->release();
    waiter.join();
    expect_ok("G13e a waiter whose slot frees inside the 1 s window", waited);
    check(waited.ms >= 250 && waited.ms < 1000, "G13e the waiter was admitted after the slot freed, before the bound (" +
                                                    std::to_string(waited.ms) + " ms)");
    for (auto& h : holders) h->release();
    check(wait_for_global(0), "G13e released");
}

// ---- G13 j / h3: key structure, atomicity, slot independence from limits -------------------------------------------
void g13_j_h3_keys_and_atomicity() {
    LimitsRestore restore;
    auto& l = agentengine::sandbox::egress_limits();
    {  // j: structured keys. tenant "a/b" + principal "c" vs tenant "a" + principal "b/c" are different buckets.
        l.per_run.store(1000);
        l.per_principal.store(1);
        l.global.store(1000);
        auto first  = egress::egress_admission_try(key("a/b", "c", "run-1"));
        auto second = egress::egress_admission_try(key("a", "b/c", "run-2"));
        check(first != nullptr, "G13j the first slot is admitted");
        check(second != nullptr, "G13j tenant a/b + root c and tenant a + root b/c are SEPARATE principal buckets");
        auto third = egress::egress_admission_try(key("a/b", "c", "run-3"));
        check(third == nullptr, "G13j the same (tenant, principal) is held to per_principal = 1");
        first.reset();
        second.reset();
        auto const snap = egress::egress_admission_snapshot();
        check(snap.global == 0 && snap.runs == 0 && snap.principals == 0, "G13j every map is empty after release");
    }
    {  // j: the run level is structured too: (tenant, principal, run) tuples never collide through concatenation.
        l.per_run.store(1);
        l.per_principal.store(1000);
        auto first  = egress::egress_admission_try(key("a/b", "c", "d"));
        auto second = egress::egress_admission_try(key("a", "b/c", "d"));
        check(first != nullptr && second != nullptr, "G13j run keys (a/b, c, d) and (a, b/c, d) are separate buckets");
    }
    {  // all-or-nothing: a call refused at the principal level leaves NO run-level residue
        l.per_run.store(8);
        l.per_principal.store(1);
        auto held   = egress::egress_admission_try(key("t", "p", "X"));
        auto refused = egress::egress_admission_try(key("t", "p", "Y"));
        check(held != nullptr && refused == nullptr, "G13h3 the second run of a full principal is refused");
        auto const snap = egress::egress_admission_snapshot();
        check(snap.global == 1 && snap.runs == 1 && snap.principals == 1,
              "G13h3 a refused call incremented nothing (no partial admission, no residue)");
    }
    {  // a slot released after the limits were lowered still decrements
        l.per_run.store(8);
        l.per_principal.store(32);
        l.global.store(64);
        auto held = egress::egress_admission_try(key("t", "q", "Z"));
        check(held != nullptr, "G13h3 admitted under the default limits");
        l.per_run.store(0);
        l.per_principal.store(0);
        l.global.store(0);
        held.reset();
        auto const snap = egress::egress_admission_snapshot();
        check(snap.global == 0 && snap.runs == 0 && snap.principals == 0,
              "G13h3 a slot released after EgressLimits was lowered still decrements");
    }
    {  // h3: under hammering, the observed in-flight never exceeds the caps
        l.per_run.store(8);
        l.per_principal.store(1000);
        l.global.store(1000);
        std::atomic<int> inflight{0};
        std::atomic<int> max_seen{0};
        std::atomic<int> admitted{0};
        {
            std::vector<std::jthread> threads;
            for (int t = 0; t < 32; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < 3000; ++i) {
                        auto h = egress::egress_admission_try(key("t", "hammer", "run"));
                        if (!h) continue;
                        int const now = inflight.fetch_add(1) + 1;
                        int seen      = max_seen.load();
                        while (now > seen && !max_seen.compare_exchange_weak(seen, now)) {}
                        admitted.fetch_add(1);
                        std::this_thread::yield();
                        inflight.fetch_sub(1);
                    }
                });
            }
        }
        check(max_seen.load() <= 8, "G13h3 under 32 hammering threads the in-flight count never exceeded the per-run cap (" +
                                        std::to_string(max_seen.load()) + ")");
        check(admitted.load() > 0, "G13h3 the hammer admitted calls (the cap is not a permanent refusal)");
        auto const snap = egress::egress_admission_snapshot();
        check(snap.global == 0 && snap.runs == 0 && snap.principals == 0, "G13h3 the counters returned to zero");
    }
}

// ---- G7: destroying the host Runtime, and a when_all sibling failure, cancel a parked fetch ---------------------------
agentengine::task<void> drive_fetch(std::shared_ptr<TestEgressProxy> p, std::uint16_t port,
                                    std::shared_ptr<std::atomic<int>> outcome) {
    auto r = co_await p->fetch(get_root(), grant_to(port), EgressCallControl{});
    outcome->store(r.has_value() ? 1 : (r.error().klass == failure_class::canceled ? 2 : 3));
}

agentengine::task<void> boom(agentengine::pal::Reactor& reactor) {
    (void)co_await agentengine::rt::sleep_for(reactor, milliseconds(150));
    throw std::runtime_error("sibling failure");
}

agentengine::task<void> fetch_and_boom(agentengine::pal::Reactor* reactor, std::shared_ptr<TestEgressProxy> p,
                                       std::uint16_t port, std::shared_ptr<std::atomic<int>> outcome,
                                       std::shared_ptr<std::atomic<bool>> threw) {
    try {
        (void)co_await agentengine::rt::when_all(drive_fetch(std::move(p), port, outcome), boom(*reactor));
    } catch (std::exception const&) {
        threw->store(true);
    }
}

void g7_runtime_and_when_all(SilentListener& silent) {
    {  // destroy the host Runtime while a fetch is parked: canceled, well under a second
        auto p       = std::make_shared<TestEgressProxy>();
        configure(*p);
        auto outcome = std::make_shared<std::atomic<int>>(0);
        long long ms = 0;
        {
            agentengine::rt::RuntimeConfig cfg;
            cfg.lanes = 1;
            auto host   = std::make_unique<agentengine::rt::Runtime>(cfg);
            auto handle = host->spawn(drive_fetch(p, silent.port(), outcome));
            check(wait_for_global(1), "G7 the host-Runtime fetch is parked (admitted, waiting on the silent peer)");
            std::this_thread::sleep_for(milliseconds(150));
            auto const t0 = clk::now();
            // ~Runtime cancels every root before joining it; the fetch must observe that through rt::scope_stop_token().
            host.reset();
            ms = std::chrono::duration_cast<milliseconds>(clk::now() - t0).count();
            (void)handle;
        }
        check(outcome->load() == 2, "G7 destroying the host Runtime cancels the parked fetch (outcome " +
                                        std::to_string(outcome->load()) + ")");
        check(ms < 1000, "G7 the host Runtime destroyed in < 1 s with a fetch parked (" + std::to_string(ms) + " ms)");
        check(wait_for_global(0), "G7 the slot was released");
    }
    {  // a when_all sibling failure cancels a parked fetch
        auto p       = std::make_shared<TestEgressProxy>();
        configure(*p);
        auto outcome = std::make_shared<std::atomic<int>>(0);
        auto threw   = std::make_shared<std::atomic<bool>>(false);
        agentengine::rt::RuntimeConfig cfg;
        cfg.lanes = 1;
        agentengine::rt::Runtime host(cfg);
        auto const t0     = clk::now();
        auto       handle = host.spawn(fetch_and_boom(&host.reactor(), p, silent.port(), outcome, threw));
        auto const end    = t0 + std::chrono::seconds(5);
        while (outcome->load() == 0 && clk::now() < end) std::this_thread::sleep_for(milliseconds(5));
        auto const ms = std::chrono::duration_cast<milliseconds>(clk::now() - t0).count();
        check(outcome->load() == 2, "G7 a when_all sibling failure cancels the parked fetch (outcome " +
                                        std::to_string(outcome->load()) + ")");
        check(ms < 1500, "G7 the sibling failure reached the fetch promptly (" + std::to_string(ms) + " ms)");
        (void)handle;
    }
}

// ---- G13g: the deadline timer fires as the host Runtime tears the frame down (run under ASan on Linux) ---------------
agentengine::task<void> drive_fetch_until(std::shared_ptr<TestEgressProxy> p, std::uint16_t port, clk::time_point deadline,
                                          std::shared_ptr<std::atomic<int>> outcome) {
    EgressCallControl c;
    c.deadline = deadline;
    auto r     = co_await p->fetch(get_root(), grant_to(port), std::move(c));
    outcome->store(r.has_value() ? 1 : (r.error().klass == failure_class::canceled ? 2 : 3));
}

void g13_g_timer_vs_teardown(SilentListener& silent) {
    // Each iteration parks a fetch on the silent peer with a 40 ms deadline and destroys its host Runtime at 40 ms +/- a
    // few ms, so across the sweep the egress-side timer completes before, during and after ~Runtime cancels and joins the
    // root. The assertion that matters is the sanitizer's (no use-after-free of the frame or the host strand); the checks
    // here pin that every call ended (canceled or deadline, never success) and every slot came back.
    constexpr int kIterations = 60;
    int           ended       = 0;
    for (int i = 0; i < kIterations; ++i) {
        auto p = std::make_shared<TestEgressProxy>();
        configure(*p);
        auto outcome = std::make_shared<std::atomic<int>>(0);
        {
            agentengine::rt::RuntimeConfig cfg;
            cfg.lanes = 1;
            auto       host     = std::make_unique<agentengine::rt::Runtime>(cfg);
            auto const deadline = clk::now() + milliseconds(40);
            auto       handle   = host->spawn(drive_fetch_until(p, silent.port(), deadline, outcome));
            std::this_thread::sleep_until(deadline + milliseconds((i % 11) - 5));
            host.reset();
            (void)handle;
        }
        if (outcome->load() == 2 || outcome->load() == 3) ++ended;
    }
    check(ended == kIterations, "G13g every fetch ended canceled or on its deadline (" + std::to_string(ended) + "/" +
                                    std::to_string(kIterations) + ")");
    check(wait_for_global(0), "G13g every slot was released");
}

}  // namespace

int main() {
    (void)agentengine::test_support::cap_process_memory(std::size_t{1024} << 20, std::size_t{4096} << 20);
    SilentListener silent;
    FastServer     fast;
    check(silent.port() != 0 && fast.port() != 0, "the loopback fixtures started");

    g13_j_h3_keys_and_atomicity();
    g13_h_run_level(silent, fast);
    g13_i_principal_level(silent, fast);
    g13_k_unattributed(silent, fast);
    g13_e_global_cap(silent, fast);
    g7_runtime_and_when_all(silent);
    g13_g_timer_vs_teardown(silent);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_net_egress_admission: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_net_egress_admission: %d FAILURE(S)\n", g_failures);
    return 1;
}
