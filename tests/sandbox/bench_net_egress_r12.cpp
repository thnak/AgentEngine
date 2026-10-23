// MANUAL measurement, NOT a CI gate: decisions/archive/ADR-241-egress-proxy-async-path-on-rt-http.md R12 / SS8.2 ("the 64 x 16 MiB
// over 2 lanes figure is a prove-step measurement, not an argument").
//
// Claim measured: sync-edge (WASM) calls run their continuations on the 2 egress lanes, so a flood of `fetch_blocking`
// calls that each receive a large body serialises the response parsing on those two lanes; a delivered cancel, or one
// more small fetch, queues behind it.
//
// Usage:  bench_net_egress_r12 <N> [body_mib=16]       (env AE_R12_CAP_MIB = process memory cap, default 2048)
//   N concurrent `fetch_blocking` calls, each answered with a `body_mib` MiB Content-Length body by a loopback server
//   (a thread per connection, sending from ONE shared buffer). Reported: wall time of the whole batch, per-call latency
//   (min/median/max), peak resident set (Windows PeakWorkingSetSize / POSIX ru_maxrss), and, while the big calls are in
//   flight, (a) the latency of one extra SMALL fetch and (b) the time a mid-flight stop takes to return `canceled`;
//   both against a baseline taken with nothing else in flight. One N per process: the peak RSS is a process-lifetime
//   number and the egress context is a leaked singleton.
// Run it once per N (8, 32, 64). Not registered in the default ctest run (the test is DISABLED; run the exe directly).
// The host limits are raised to what the run needs (global 256, per-run/principal 256): the admission caps (R1) are not
// what is being measured, and each call gets its OWN principal and root-run key.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif

#include "../support/crt_fail_fast.hpp"
#include "../support/memory_cap.hpp"
#include "agentengine/pal/net.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/sandbox/testing/net_egress_proxy_testing.hpp"

namespace {

using namespace agentengine;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using agentengine::sandbox::EgressCallControl;
using agentengine::sandbox::NetEgressRequest;
using agentengine::sandbox::NetEgressResponse;
using agentengine::sandbox::TestEgressProxy;

std::uint64_t peak_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
    return 0;
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024u;
#endif
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// A loopback server: accepts, then serves each connection on its own thread. `body_bytes` of 'x' from one shared buffer;
// `hold` = never answer (a silent peer, for the cancel probe).
class BulkServer {
public:
    BulkServer(std::size_t body_bytes, bool hold) : body_(body_bytes), hold_(hold) {
        auto l = pal::tcp_listen(0x7F000001, 0, 256);
        if (l) {
            listen_ = *l;
            port_   = pal::local_port(listen_).value_or(0);
        }
        head_   = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body_bytes) + "\r\nConnection: close\r\n\r\n";
        shared_ = std::string(std::min<std::size_t>(body_bytes, 1u << 20), 'x');
        accept_ = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~BulkServer() {
        accept_.request_stop();
        accept_.join();
        std::lock_guard lock(m_);
        for (auto& t : conns_) t.request_stop();
        conns_.clear();  // jthread dtor joins
        pal::close_fd(listen_);
    }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int           accepted() const { return accepted_.load(); }

private:
    static void send_all(pal::fd_t fd, char const* p, std::size_t n, std::stop_token const& st) {
        std::size_t off = 0;
        while (off < n && !st.stop_requested()) {
            auto r = pal::send_some(fd, reinterpret_cast<std::byte const*>(p) + off, n - off);
            if (!r) {
                if (r.error() == pal::would_block()) {
                    std::this_thread::sleep_for(200us);
                    continue;
                }
                return;
            }
            off += *r;
        }
    }
    void serve(pal::fd_t fd, std::stop_token st) {
        std::string head;
        std::byte   buf[4096];
        auto const  end = Clock::now() + 60s;
        while (head.find("\r\n\r\n") == std::string::npos && !st.stop_requested() && Clock::now() < end) {
            auto r = pal::recv_some(fd, buf, sizeof(buf));
            if (!r) {
                if (r.error() == pal::would_block()) {
                    std::this_thread::sleep_for(200us);
                    continue;
                }
                break;
            }
            if (*r == 0) break;
            head.append(reinterpret_cast<char const*>(buf), *r);
        }
        if (hold_) {
            while (!st.stop_requested() && Clock::now() < end) std::this_thread::sleep_for(5ms);
        } else if (head.find("\r\n\r\n") != std::string::npos) {
            send_all(fd, head_.data(), head_.size(), st);
            std::size_t left = body_;
            while (left > 0 && !st.stop_requested()) {
                std::size_t const n = std::min(left, shared_.size());
                send_all(fd, shared_.data(), n, st);
                left -= n;
            }
        }
        pal::close_fd(fd);
    }
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = pal::accept_one(listen_);
            if (!a) {
                std::this_thread::sleep_for(500us);
                continue;
            }
            accepted_.fetch_add(1);
            pal::fd_t fd = *a;
            std::lock_guard lock(m_);
            conns_.emplace_back([this, fd](std::stop_token cst) { serve(fd, cst); });
        }
    }

    std::size_t               body_;
    bool                      hold_;
    std::string               head_;
    std::string               shared_;
    pal::fd_t                 listen_ = pal::invalid_fd;
    std::uint16_t             port_   = 0;
    std::atomic<int>          accepted_{0};
    std::mutex                m_;
    std::vector<std::jthread> conns_;
    std::jthread              accept_;
};

pal::ResolveOutcome loopback_dns(std::string const&) {
    pal::ResolveOutcome o;
    o.addresses.push_back(pal::IpAddress::loopback_v4());
    return o;
}
bool permit_loopback(pal::IpAddress const& a) noexcept { return a == pal::IpAddress::loopback_v4(); }

void configure(TestEgressProxy& p) {
    p.host_resolver  = loopback_dns;
    p.address_policy = permit_loopback;
    p.total_timeout  = 120s;
}

cap::NetOut grant(std::uint16_t port) {
    cap::NetOut g;
    g.host_allowlist = {"bulk.example:" + std::to_string(port) + ":http"};
    return g;
}

EgressCallControl control(std::string const& tag, std::stop_token st = {}) {
    EgressCallControl c;
    c.stop              = std::move(st);
    c.admission.tenant  = "bench";
    c.admission.principal = "p-" + tag;  // a distinct principal and run per call: the admission caps are not measured
    c.admission.root_run  = "r-" + tag;
    return c;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    agentengine::test_support::fail_fast_on_windows();
    std::size_t const n = argc > 1 ? static_cast<std::size_t>(std::atoi(argv[1])) : 8;
    std::size_t const body_mib = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 16;
    std::size_t cap_mib = 2048;
#if defined(_WIN32)
    {
        char   buf[32];
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), "AE_R12_CAP_MIB") == 0 && len > 0) cap_mib = static_cast<std::size_t>(std::atoll(buf));
    }
#else
    if (char const* e = std::getenv("AE_R12_CAP_MIB")) cap_mib = static_cast<std::size_t>(std::atoll(e));
#endif
    (void)agentengine::test_support::cap_process_memory(cap_mib << 20, cap_mib << 21);
    std::thread([] {
        std::this_thread::sleep_for(280s);
        std::printf("WATCHDOG: the bench did not finish within 280 s\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();
    pal::ensure_winsock();

    auto& lim = sandbox::egress_limits();
    lim.global.store(256);
    lim.per_run.store(256);
    lim.per_principal.store(256);

    std::size_t const body_bytes = body_mib << 20;
    BulkServer        big(body_bytes, false);
    BulkServer        small(2, false);
    BulkServer        silent(0, true);
    TestEgressProxy proxy;
    configure(proxy);
    auto const small_grant = grant(small.port());
    auto const silent_grant = grant(silent.port());

    auto const small_fetch = [&](std::string const& tag) {
        auto const t0 = Clock::now();
        auto       r  = proxy.fetch_blocking(NetEgressRequest{"GET", "/", {}, ""}, small_grant, control(tag));
        return std::pair<bool, double>{r.has_value() && r->status == 200, ms(Clock::now() - t0)};
    };
    auto const cancel_probe = [&](std::string const& tag) {
        std::stop_source ss;
        Clock::time_point t_stop{};
        std::jthread canceller([&] {
            std::this_thread::sleep_for(100ms);
            t_stop = Clock::now();
            ss.request_stop();
        });
        auto r = proxy.fetch_blocking(NetEgressRequest{"GET", "/", {}, ""}, silent_grant, control(tag, ss.get_token()));
        auto const t_ret = Clock::now();
        canceller.join();
        bool const canceled = !r.has_value() && r.error().klass == failure_class::canceled;
        return std::pair<bool, double>{canceled, ms(t_ret - t_stop)};
    };

    // Baseline: nothing else in flight (also warms the leaked egress context).
    (void)small_fetch("warm");
    std::vector<double> base_small, base_cancel;
    for (int i = 0; i < 5; ++i) base_small.push_back(small_fetch("b" + std::to_string(i)).second);
    for (int i = 0; i < 3; ++i) base_cancel.push_back(cancel_probe("bc" + std::to_string(i)).second);

    // The batch.
    std::vector<double> lat(n, 0.0);
    std::vector<int>    ok(n, 0);
    std::vector<std::string> err(n);
    std::atomic<std::size_t> finished{0};
    std::latch               go(1);
    std::vector<std::jthread> callers;
    callers.reserve(n);
    auto const t_batch = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        callers.emplace_back([&, i] {
            go.wait();
            auto const t0 = Clock::now();
            auto       r  = proxy.fetch_blocking(NetEgressRequest{"GET", "/", {}, ""}, grant(big.port()),
                                            control("c" + std::to_string(i)));
            lat[i] = ms(Clock::now() - t0);
            ok[i]  = (r.has_value() && r->status == 200 && r->body.size() == body_bytes) ? 1 : 0;
            if (!r.has_value()) err[i] = r.error().code;
            finished.fetch_add(1);
        });
    }
    go.count_down();

    // Probes while the big calls are in flight: wait until the server has seen them all, then keep probing until done.
    std::vector<double> mid_small, mid_cancel;
    int                 mid_small_ok = 0, mid_cancel_ok = 0, mid_small_total = 0, mid_cancel_total = 0;
    {
        auto const end = Clock::now() + 60s;
        while (static_cast<std::size_t>(big.accepted()) < n && Clock::now() < end) std::this_thread::sleep_for(1ms);
        int round = 0;
        while (finished.load() < n && Clock::now() < end && round < 12) {
            auto const [ok1, t1] = small_fetch("ms" + std::to_string(round));
            ++mid_small_total;
            mid_small_ok += ok1 ? 1 : 0;
            mid_small.push_back(t1);
            if (finished.load() >= n) break;
            auto const [ok2, t2] = cancel_probe("mc" + std::to_string(round));
            ++mid_cancel_total;
            mid_cancel_ok += ok2 ? 1 : 0;
            mid_cancel.push_back(t2);
            ++round;
        }
    }
    for (auto& c : callers) c.join();
    double const wall = ms(Clock::now() - t_batch);
    std::size_t  succeeded = 0;
    for (int v : ok) succeeded += static_cast<std::size_t>(v);
    std::string first_err;
    for (auto const& e : err)
        if (!e.empty()) {
            first_err = e;
            break;
        }

    auto const mm = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? std::pair<double, double>{0, 0} : std::pair<double, double>{v.front(), v.back()};
    };
    auto const [lmin, lmax] = mm(lat);
    std::printf("R12 N=%zu body=%zu MiB cap=%zu MiB (%s)\n", n, body_mib, cap_mib,
#if defined(NDEBUG)
                "NDEBUG"
#else
                "Debug"
#endif
    );
    std::printf("  batch: %zu/%zu calls ok (first error: %s)  wall %.0f ms  per-call latency min/median/max %.0f / %.0f / %.0f ms\n",
                succeeded, n, first_err.empty() ? "none" : first_err.c_str(), wall, lmin, median(lat), lmax);
    std::printf("  peak RSS (whole process, incl. loopback peers): %.0f MiB\n", static_cast<double>(peak_rss_bytes()) / (1 << 20));
    auto const [bs0, bs1] = mm(base_small);
    auto const [bc0, bc1] = mm(base_cancel);
    std::printf("  extra small fetch, baseline (idle):      median %.1f ms (min %.1f, max %.1f)\n", median(base_small), bs0, bs1);
    auto const [ms0, ms1] = mm(mid_small);
    std::printf("  extra small fetch, while big in flight:  median %.1f ms (min %.1f, max %.1f) over %d probes, %d ok\n",
                median(mid_small), ms0, ms1, mid_small_total, mid_small_ok);
    std::printf("  stop -> canceled return, baseline (idle): median %.1f ms (min %.1f, max %.1f)\n", median(base_cancel), bc0, bc1);
    auto const [mc0, mc1] = mm(mid_cancel);
    std::printf("  stop -> canceled return, big in flight:  median %.1f ms (min %.1f, max %.1f) over %d probes, %d canceled\n",
                median(mid_cancel), mc0, mc1, mid_cancel_total, mid_cancel_ok);
    std::fflush(stdout);
    return succeeded == n ? 0 : 1;
}
