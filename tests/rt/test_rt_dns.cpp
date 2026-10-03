// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("DNS") and §4.6 ("DNS gets its own
// bounded pool", the offload residual), with ADR-011's resolve-once / connect-to-the-verified-address rule --
// rt::resolve and rt::connect_resolved (rt/dns.hpp) over pal::resolve_host_blocking (pal/resolver.hpp). Each
// claim names the rule it proves and the mutant that kills it. No claim depends on the network: slow, failing
// and scripted lookups go through the `HostResolver` test seam; only D1 asks the real resolver, for "localhost".
//
//   D1  resolve("localhost") through getaddrinfo answers a loopback address; the lookup runs on a DNS-pool
//       worker and the caller resumes on its HOME (the block_on thread), never on the worker (§4.2/§4.6).
//       Mutant: resume inline from the worker (offload deliver) -> resumed_on is the worker.
//   D2  A numeric host ("127.0.0.1", "::1", "fe80::1%1") skips DNS: the resolver is never called, no thread hop.
//       Mutant: drop the numeric fast path -> the seam counts a call.
//   D3  A stop during a slow lookup resumes the caller `canceled` PROMPTLY, on its home, while the lookup keeps
//       running in the pool; its late answer is discarded and counted (§4.6 residual). Mutant: the stop is not
//       passed to the offload -> the caller waits for the lookup.
//   D4  A stop already requested completes `canceled` and the resolver never runs.
//   D5  Resolver outcomes are values: not_found / temporary / resource map through; a resolver that throws is
//       `other`; an invalid host (empty, NUL, > 253 chars) is `invalid_argument` without a lookup.
//   D6  connect_resolved without a policy is `no_policy` -- refused before any lookup (fail closed, I2).
//       Mutant: treat an empty policy as allow-all -> it connects.
//   D7  A policy that rejects every address -> `every_address_rejected`, NOTHING is connected (the listener
//       sees no connection), and the policy saw every resolved address. Positive control: the same lookup with
//       an accepting policy connects. Mutant: ignore the policy's answer -> D7 connects.
//   D8  A policy that throws is a rejection (fail closed).
//   D9  Only accepted addresses are tried, in the resolver's order: [rejected, refused, good] -> one rejection,
//       a refused attempt, then a connection to the good address, which `address` reports.
//   D10 The per-attempt deadline: a first address whose connect does not complete (a listener whose backlog is
//       full, on Linux; a non-routable TEST-NET address elsewhere) is abandoned after `attempt_timeout` and the
//       next address connects. Mutant: never start the deadline timer -> the attempt outlives the bound.
//   D11 A caller stop during a pending connect resumes `canceled` promptly (not `timed_out`), and during the
//       lookup of connect_resolved likewise.
//
// Coroutines are free functions taking their state by value or pointer, never immediately-invoked lambda
// coroutines with captures (ADR-237 §14). Result containers are declared before the reactor/pools that write
// into them.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"
#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/resolver.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/dns.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/task.hpp"
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

bool is_loopback(pal::IpAddress const& a) {
    return (a.family == pal::IpAddress::family_kind::v4 && a.bytes[0] == 127) || a == pal::IpAddress::loopback_v6();
}

// A scripted resolver's shared record (the seam is copied into the job by value, so it holds this by pointer).
struct SeamLog {
    std::atomic<int>             calls{0};
    std::atomic<bool>            finished{false};
    std::atomic<bool>            release{false};
    std::mutex                   m;
    std::thread::id              ran_on{};
    std::vector<std::string>     hosts;
};

HostResolver scripted(std::shared_ptr<SeamLog> log, pal::ResolveOutcome answer,
                      std::chrono::milliseconds hold = 0ms) {
    return [log = std::move(log), answer = std::move(answer), hold](std::string const& host) {
        log->calls.fetch_add(1);
        {
            std::lock_guard lock(log->m);
            log->ran_on = std::this_thread::get_id();
            log->hosts.push_back(host);
        }
        auto const end = std::chrono::steady_clock::now() + hold;
        while (std::chrono::steady_clock::now() < end && !log->release.load()) std::this_thread::sleep_for(1ms);
        log->finished.store(true);
        return answer;
    };
}

HostResolver real_resolver_logged(std::shared_ptr<SeamLog> log) {
    return [log = std::move(log)](std::string const& host) {
        log->calls.fetch_add(1);
        {
            std::lock_guard lock(log->m);
            log->ran_on = std::this_thread::get_id();
        }
        return pal::resolve_host_blocking(host);
    };
}

pal::ResolveOutcome answer_of(std::vector<pal::IpAddress> addrs) {
    pal::ResolveOutcome o;
    o.addresses = std::move(addrs);
    return o;
}
pal::ResolveOutcome failure_of(pal::resolve_error e) {
    pal::ResolveOutcome o;
    o.error = e;
    return o;
}

// ---- coroutines -------------------------------------------------------------------------------------------

struct Observed {
    DnsResult       r;
    std::thread::id resumed_on{};
};

task<Observed> observe_resolve(OffloadPool* pool, std::string host, std::uint16_t port, std::stop_token stop,
                               HostResolver resolver) {
    Observed o;
    o.r          = co_await resolve(*pool, std::move(host), port, std::move(stop), std::move(resolver));
    o.resumed_on = std::this_thread::get_id();
    co_return o;
}

struct ObservedConnect {
    ConnectedStream c;
    std::thread::id resumed_on{};
};

task<ObservedConnect> observe_connect(pal::Reactor* reactor, OffloadPool* pool, std::string host,
                                      std::uint16_t port, AddressPolicy policy, std::stop_token stop,
                                      ConnectOptions options) {
    ObservedConnect o;
    o.c = co_await connect_resolved(*reactor, *pool, std::move(host), port, std::move(policy), std::move(stop),
                                    std::move(options));
    o.resumed_on = std::this_thread::get_id();
    co_return o;
}

// ---- loopback listener ------------------------------------------------------------------------------------

struct Listener {
    pal::fd_t     fd   = pal::invalid_fd;
    std::uint16_t port = 0;
    explicit Listener(int backlog = 64) {
        auto l = pal::tcp_listen(kLoopback, 0, backlog);
        if (!l) return;
        fd   = *l;
        port = pal::local_port(fd).value_or(0);
    }
    ~Listener() { pal::close_fd(fd); }
    Listener(Listener const&)            = delete;
    Listener& operator=(Listener const&) = delete;

    // How many connections arrive within `limit` (each accepted one is closed again).
    int accepted_within(std::chrono::milliseconds limit) const {
        int        n   = 0;
        auto const end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            auto a = pal::accept_one(fd);
            if (a) {
                ++n;
                pal::close_fd(*a);
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        return n;
    }
};

// Requests a stop after `delay`, from its own thread; joined on destruction.
class DelayedStop {
public:
    DelayedStop(std::stop_source src, std::chrono::milliseconds delay)
        : thread_([src, delay]() mutable {
              std::this_thread::sleep_for(delay);
              src.request_stop();
          }) {}
    ~DelayedStop() { thread_.join(); }
    DelayedStop(DelayedStop const&)            = delete;
    DelayedStop& operator=(DelayedStop const&) = delete;

private:
    std::thread thread_;
};

AddressPolicy allow_all() {
    return [](pal::IpAddress const&) { return true; };
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

    std::thread::id const home = std::this_thread::get_id();
    auto                  reactor = pal::make_default_reactor();
    OffloadPool           dns{{.workers = 2}};  // the dedicated DNS pool (§4.6)

    // D1 ------------------------------------------------------------------------------------------------
    {
        auto     log = std::make_shared<SeamLog>();
        Observed o   = block_on(observe_resolve(&dns, "localhost", 443, {}, real_resolver_logged(log)));
        bool any_loopback = false;
        for (auto const& a : o.r.addresses) any_loopback = any_loopback || is_loopback(a);
        check(o.r.ok() && any_loopback && !o.r.numeric && o.r.port == 443,
              "D1: resolve(\"localhost\") through getaddrinfo answers a loopback address");
        std::thread::id ran_on;
        {
            std::lock_guard lock(log->m);
            ran_on = log->ran_on;
        }
        check(log->calls.load() == 1 && ran_on != home && ran_on != std::thread::id{},
              "D1: the lookup ran on a DNS-pool worker, not the caller's thread");
        check(o.resumed_on == home, "D1: the caller resumed on its home (the block_on thread), not the worker");
        pal::ResolveOutcome const direct = pal::resolve_host_blocking("localhost");
        check(direct.error == pal::resolve_error::none && !direct.addresses.empty(),
              "D1: pal::resolve_host_blocking answers localhost directly too");
    }

    // D2 ------------------------------------------------------------------------------------------------
    {
        auto log = std::make_shared<SeamLog>();
        auto seam = scripted(log, answer_of({pal::IpAddress::v4(10, 9, 8, 7)}));
        Observed a = block_on(observe_resolve(&dns, "127.0.0.1", 80, {}, seam));
        Observed b = block_on(observe_resolve(&dns, "::1", 80, {}, seam));
        Observed c = block_on(observe_resolve(&dns, "fe80::1%1", 80, {}, seam));
        check(a.r.ok() && a.r.numeric && a.r.addresses.size() == 1 && a.r.addresses[0] == pal::IpAddress::loopback_v4(),
              "D2: \"127.0.0.1\" is numeric: answered as itself");
        check(b.r.ok() && b.r.numeric && b.r.addresses.size() == 1 && b.r.addresses[0] == pal::IpAddress::loopback_v6(),
              "D2: \"::1\" is numeric: answered as itself");
        check(c.r.ok() && c.r.numeric && c.r.addresses.size() == 1 &&
                  c.r.addresses[0].family == pal::IpAddress::family_kind::v6,
              "D2: a scoped link-local v6 literal is numeric too");
        check(log->calls.load() == 0, "D2: the resolver was never called for a numeric host (no DNS, no thread hop)");
        check(a.resumed_on == home, "D2: ... and the caller never left its thread");
    }

    // D3 ------------------------------------------------------------------------------------------------
    {
        auto             log = std::make_shared<SeamLog>();
        std::uint64_t const discarded_before = dns.discarded_results();
        std::stop_source src;
        auto const       t0 = std::chrono::steady_clock::now();
        Observed         o;
        {
            DelayedStop stopper(src, 50ms);
            o = block_on(observe_resolve(&dns, "slow.example", 443, src.get_token(),
                                         scripted(log, answer_of({pal::IpAddress::v4(10, 0, 0, 1)}), 3000ms)));
        }
        auto const dt = std::chrono::steady_clock::now() - t0;
        check(o.r.error == dns_error::canceled && o.r.addresses.empty(),
              "D3: a stop during a slow lookup resumes the caller `canceled`");
        check(dt < 1500ms, "D3: ... promptly (" +
                               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()) +
                               " ms; the lookup holds for 3000 ms)");
        check(o.resumed_on == home, "D3: ... on its home");
        check(log->calls.load() == 1 && !log->finished.load(),
              "D3: the lookup was still running in the pool when the caller resumed (§4.6 residual)");
        log->release.store(true);
        check(wait_until([&] { return dns.discarded_results() == discarded_before + 1; }, 5s),
              "D3: the lookup finished in the pool and its late answer was discarded and counted");
    }

    // D4 ------------------------------------------------------------------------------------------------
    {
        auto             log = std::make_shared<SeamLog>();
        std::stop_source src;
        src.request_stop();
        Observed o = block_on(observe_resolve(&dns, "example.com", 443, src.get_token(),
                                              scripted(log, answer_of({pal::IpAddress::v4(10, 0, 0, 1)}))));
        std::this_thread::sleep_for(20ms);
        check(o.r.error == dns_error::canceled && log->calls.load() == 0,
              "D4: a stop already requested completes `canceled`; the resolver never runs");
    }

    // D5 ------------------------------------------------------------------------------------------------
    {
        auto     log = std::make_shared<SeamLog>();
        Observed nf  = block_on(observe_resolve(&dns, "nx.example", 1, {}, scripted(log, failure_of(pal::resolve_error::not_found))));
        Observed tmp = block_on(observe_resolve(&dns, "tmp.example", 1, {}, scripted(log, failure_of(pal::resolve_error::temporary))));
        Observed res = block_on(observe_resolve(&dns, "res.example", 1, {}, scripted(log, failure_of(pal::resolve_error::resource))));
        Observed empty_ok = block_on(observe_resolve(&dns, "empty.example", 1, {}, scripted(log, answer_of({}))));
        check(nf.r.error == dns_error::not_found && tmp.r.error == dns_error::temporary &&
                  res.r.error == dns_error::resource,
              "D5: not_found / temporary / resource map through as values");
        check(empty_ok.r.error == dns_error::not_found, "D5: an empty answer is `not_found`, never an empty `ok`");
        HostResolver throwing = [](std::string const&) -> pal::ResolveOutcome { throw std::runtime_error("boom"); };
        Observed thrown = block_on(observe_resolve(&dns, "throw.example", 1, {}, throwing));
        check(thrown.r.error == dns_error::other, "D5: a resolver that throws is `other`, delivered as a value");
        int const before = log->calls.load();
        Observed e1 = block_on(observe_resolve(&dns, "", 1, {}, scripted(log, answer_of({}))));
        Observed e2 = block_on(observe_resolve(&dns, std::string("a\0b", 3), 1, {}, scripted(log, answer_of({}))));
        Observed e3 = block_on(observe_resolve(&dns, std::string(254, 'a'), 1, {}, scripted(log, answer_of({}))));
        check(e1.r.error == dns_error::invalid_argument && e2.r.error == dns_error::invalid_argument &&
                  e3.r.error == dns_error::invalid_argument && log->calls.load() == before,
              "D5: an empty / NUL-bearing / over-long host is `invalid_argument`, with no lookup");
        check(pal::resolve_host_blocking("").error == pal::resolve_error::invalid_argument,
              "D5: the blocking resolver refuses an empty host too");
    }

    // D6 ------------------------------------------------------------------------------------------------
    {
        Listener        l;
        auto            log = std::make_shared<SeamLog>();
        ConnectOptions  opts;
        opts.resolver = scripted(log, answer_of({pal::IpAddress::loopback_v4()}));
        ObservedConnect o = block_on(observe_connect(reactor.get(), &dns, "svc.example", l.port, AddressPolicy{}, {}, opts));
        check(o.c.error == connect_error::no_policy && !o.c.stream, "D6: no policy -> `no_policy`, nothing connected");
        check(log->calls.load() == 0, "D6: ... refused before any lookup");
        check(l.accepted_within(200ms) == 0, "D6: ... and the listener saw no connection");
    }

    // D7 ------------------------------------------------------------------------------------------------
    {
        Listener                    l;
        auto                        log = std::make_shared<SeamLog>();
        auto                        seen = std::make_shared<std::vector<pal::IpAddress>>();
        ConnectOptions              opts;
        std::vector<pal::IpAddress> const answer{pal::IpAddress::loopback_v4(), pal::IpAddress::loopback_v6()};
        opts.resolver = scripted(log, answer_of(answer));
        AddressPolicy reject_all = [seen](pal::IpAddress const& a) {
            seen->push_back(a);
            return false;
        };
        ObservedConnect o = block_on(observe_connect(reactor.get(), &dns, "svc.example", l.port, reject_all, {}, opts));
        check(o.c.error == connect_error::every_address_rejected && !o.c.stream && o.c.attempts.empty(),
              "D7: a policy rejecting every address -> `every_address_rejected`, no attempt made");
        check(o.c.rejected == 2 && *seen == answer, "D7: the policy was asked about every resolved address, in order");
        check(l.accepted_within(300ms) == 0, "D7: NOTHING connected (the listener saw no connection)");
        check(o.resumed_on == home, "D7: resumed on its home");
        // Positive control: the same lookup, an accepting policy.
        ConnectOptions opts2;
        opts2.resolver   = scripted(log, answer_of({pal::IpAddress::loopback_v4()}));
        ObservedConnect ok = block_on(observe_connect(reactor.get(), &dns, "svc.example", l.port, allow_all(), {}, opts2));
        check(ok.c.ok() && ok.c.stream && ok.c.address == pal::IpAddress::loopback_v4() && ok.c.attempts.size() == 1,
              "D7 control: with an accepting policy it connects to the verified address");
        check(l.accepted_within(300ms) == 1, "D7 control: ... and the listener saw exactly that connection");
        check(ok.resumed_on == home, "D7 control: resumed on its home");
    }

    // D8 ------------------------------------------------------------------------------------------------
    {
        Listener       l;
        auto           log = std::make_shared<SeamLog>();
        ConnectOptions opts;
        opts.resolver           = scripted(log, answer_of({pal::IpAddress::loopback_v4()}));
        AddressPolicy throwing = [](pal::IpAddress const&) -> bool { throw std::runtime_error("policy bug"); };
        ObservedConnect o = block_on(observe_connect(reactor.get(), &dns, "svc.example", l.port, throwing, {}, opts));
        check(o.c.error == connect_error::every_address_rejected && !o.c.stream && o.c.rejected == 1,
              "D8: a policy that throws rejects (fail closed)");
        check(l.accepted_within(200ms) == 0, "D8: ... nothing connected");
    }

    // D9 ------------------------------------------------------------------------------------------------
    {
        Listener      good;
        std::uint16_t port = good.port;
        auto          log  = std::make_shared<SeamLog>();
        // 192.0.2.1 (TEST-NET-1) is rejected by the policy; ::1 is accepted but nobody listens there on this
        // port (the listener is IPv4 only) -> refused; 127.0.0.1 is accepted and connects.
        std::vector<pal::IpAddress> const answer{pal::IpAddress::v4(192, 0, 2, 1), pal::IpAddress::loopback_v6(),
                                                 pal::IpAddress::loopback_v4()};
        ConnectOptions opts;
        opts.resolver = scripted(log, answer_of(answer));
        AddressPolicy loopback_only = [](pal::IpAddress const& a) { return is_loopback(a); };
        ObservedConnect o = block_on(observe_connect(reactor.get(), &dns, "svc.example", port, loopback_only, {}, opts));
        check(o.c.ok() && o.c.address == pal::IpAddress::loopback_v4(), "D9: connected to the accepted, working address");
        check(o.c.rejected == 1, "D9: the TEST-NET address was rejected by the policy, never tried");
        check(o.c.attempts.size() == 2 && o.c.attempts[0].address == pal::IpAddress::loopback_v6() &&
                  o.c.attempts[0].error == pal::tcp_error::connection_refused &&
                  o.c.attempts[1].address == pal::IpAddress::loopback_v4() &&
                  o.c.attempts[1].error == pal::tcp_error::none,
              "D9: accepted addresses tried in the resolver's order: ::1 refused, then 127.0.0.1");
        check(good.accepted_within(300ms) == 1, "D9: the listener saw exactly one connection");
    }

    // D10 -----------------------------------------------------------------------------------------------
    {
        Listener good;
#if !defined(_WIN32)
        // A listener with a full backlog that never accepts: Linux drops further SYNs, so a connect to it stays
        // pending -- a deterministic, local "black hole".
        Listener full(0);
        std::vector<pal::fd_t> fillers;
        for (int i = 0; i < 8; ++i) {
            auto f = pal::tcp_connect(kLoopback, full.port);
            if (f) fillers.push_back(*f);
        }
        pal::IpAddress const black_hole      = pal::IpAddress::loopback_v4();
        std::uint16_t const  black_hole_port = full.port;
#else
        pal::IpAddress const black_hole      = pal::IpAddress::v4(192, 0, 2, 1);  // TEST-NET-1, not routed
        std::uint16_t const  black_hole_port = good.port;
#endif
        auto log = std::make_shared<SeamLog>();
        ConnectOptions opts;
        opts.attempt_timeout = 300ms;
        opts.resolver        = scripted(log, answer_of({black_hole}));
        auto const      t0   = std::chrono::steady_clock::now();
        ObservedConnect o    = block_on(observe_connect(reactor.get(), &dns, "hole.example", black_hole_port, allow_all(), {}, opts));
        auto const      dt   = std::chrono::steady_clock::now() - t0;
        auto const      ms   = std::chrono::duration_cast<std::chrono::milliseconds>(dt).count();
        bool const      timed_out = o.c.attempts.size() == 1 && o.c.attempts[0].timed_out;
        if (timed_out) {
            check(o.c.error == connect_error::connect_failed && !o.c.stream,
                  "D10: an attempt that never completes is abandoned: `connect_failed`, nothing connected");
            check(o.c.attempts[0].error == pal::tcp_error::canceled && dt >= 250ms && dt < 2500ms,
                  "D10: ... after the 300 ms per-attempt deadline (" + std::to_string(ms) + " ms)");
            check(o.resumed_on == home, "D10: resumed on its home");
        } else {
            std::printf("[note] D10: the black hole answered at once on this host (%s, %lld ms); deadline not exercised\n",
                        o.c.attempts.empty() ? "no attempt" : "attempt failed fast", static_cast<long long>(ms));
            check(!o.c.ok() || o.c.attempts.size() == 1, "D10: (fallback) one attempt, reported");
        }
        // The fallback to the next address after a timeout needs the black hole and the good listener on ONE port
        // (connect_resolved takes one port): only the Windows layout (TEST-NET + 127.0.0.1) has that; on Linux the
        // ordered fallback after a failed attempt is D9's.
#if defined(_WIN32)
        ConnectOptions opts2;
        opts2.attempt_timeout = 300ms;
        opts2.resolver        = scripted(log, answer_of({black_hole, pal::IpAddress::loopback_v4()}));
        ObservedConnect o2 = block_on(observe_connect(reactor.get(), &dns, "hole.example", good.port, allow_all(), {}, opts2));
        check(o2.c.ok() && o2.c.address == pal::IpAddress::loopback_v4() && o2.c.attempts.size() == 2,
              "D10: after the first address fails, the next accepted address connects");
#else
        for (auto f : fillers) pal::close_fd(f);
#endif
    }

    // D11 -----------------------------------------------------------------------------------------------
    {
        // During the lookup of connect_resolved.
        Listener         l;
        auto             log = std::make_shared<SeamLog>();
        std::stop_source src;
        ConnectOptions   opts;
        opts.resolver = scripted(log, answer_of({pal::IpAddress::loopback_v4()}), 3000ms);
        auto const      t0 = std::chrono::steady_clock::now();
        ObservedConnect o;
        {
            DelayedStop stopper(src, 50ms);
            o = block_on(observe_connect(reactor.get(), &dns, "slow.example", l.port, allow_all(), src.get_token(), opts));
        }
        auto const dt = std::chrono::steady_clock::now() - t0;
        log->release.store(true);
        check(o.c.error == connect_error::canceled && o.c.dns.error == dns_error::canceled && !o.c.stream &&
                  dt < 1500ms,
              "D11: a stop during connect_resolved's lookup resumes `canceled` promptly");
        check(l.accepted_within(200ms) == 0, "D11: ... nothing connected");
    }
#if !defined(_WIN32)
    {
        // During a pending connect (the full-backlog black hole), with a long per-attempt deadline.
        Listener               full(0);
        std::vector<pal::fd_t> fillers;
        for (int i = 0; i < 8; ++i) {
            auto f = pal::tcp_connect(kLoopback, full.port);
            if (f) fillers.push_back(*f);
        }
        auto             log = std::make_shared<SeamLog>();
        std::stop_source src;
        ConnectOptions   opts;
        opts.attempt_timeout = 10s;
        opts.resolver        = scripted(log, answer_of({pal::IpAddress::loopback_v4()}));
        auto const      t0   = std::chrono::steady_clock::now();
        ObservedConnect o;
        {
            DelayedStop stopper(src, 100ms);
            o = block_on(observe_connect(reactor.get(), &dns, "hole.example", full.port, allow_all(), src.get_token(), opts));
        }
        auto const dt = std::chrono::steady_clock::now() - t0;
        bool const pending_connect = o.c.attempts.size() == 1 && o.c.attempts[0].error == pal::tcp_error::canceled;
        check(o.c.error == connect_error::canceled && pending_connect && !o.c.attempts[0].timed_out && dt < 2s,
              "D11: a stop during a pending connect resumes `canceled` promptly, not `timed_out` (" +
                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()) + " ms)");
        check(o.resumed_on == home, "D11: ... on its home");
        for (auto f : fillers) pal::close_fd(f);
    }
#endif

    dns.shutdown();
    reactor.reset();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
