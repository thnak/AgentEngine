// Proves decisions/ADR-241-egress-proxy-async-path-on-rt-http.md section 8.2 / G13 rows b, d and f: the process-wide egress
// I/O context is created lazily, exactly once, never by a gate-rejected call, and a failed construction is retried.
//
// These rows need a FRESH process (the context is a leaked singleton that, once built, is never rebuilt), so the executable
// takes a mode: `once` (b, then d) and `inject` (f). Loopback only; memory-capped.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <latch>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../support/memory_cap.hpp"
#include "agentengine/pal/net.hpp"
#include "agentengine/sandbox/detail/net_egress_impl.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/sandbox/testing/net_egress_proxy_testing.hpp"

namespace {

using agentengine::cap::NetOut;
using agentengine::failure_class;
using agentengine::sandbox::EgressCallControl;
using agentengine::sandbox::NetEgressRequest;
using agentengine::sandbox::TestEgressProxy;
namespace egress = agentengine::sandbox::egress_detail;
using std::chrono::milliseconds;

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

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

class FastServer {
public:
    FastServer() {
        auto l = agentengine::pal::tcp_listen(static_cast<std::uint64_t>((127u << 24) | 1u), 0);
        if (!l) return;
        fd_     = *l;
        port_   = *agentengine::pal::local_port(fd_);
        thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested()) {
                auto a = agentengine::pal::accept_one(fd_);
                if (!a) {
                    std::this_thread::sleep_for(milliseconds(1));
                    continue;
                }
                std::string buf;
                std::array<std::byte, 4096> chunk{};
                for (int i = 0; i < 400 && buf.find("\r\n\r\n") == std::string::npos; ++i) {
                    auto r = agentengine::pal::recv_some(*a, chunk.data(), chunk.size());
                    if (!r) {
                        if (r.error() == agentengine::pal::would_block()) {
                            std::this_thread::sleep_for(milliseconds(1));
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

bool permit_all(agentengine::pal::IpAddress const&) noexcept { return true; }
agentengine::pal::ResolveOutcome loopback_dns(std::string const&) {
    agentengine::pal::ResolveOutcome out;
    out.addresses.push_back(agentengine::pal::IpAddress::v4(127, 0, 0, 1));
    return out;
}

void configure(TestEgressProxy& p) {
    p.host_resolver  = loopback_dns;
    p.address_policy = permit_all;
}

NetOut grant_to(std::uint16_t port) {
    NetOut n;
    n.host_allowlist = {"test.invalid:" + std::to_string(port) + ":http"};
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    (void)agentengine::test_support::cap_process_memory(std::size_t{512} << 20, std::size_t{2048} << 20);
    std::string const mode = argc > 1 ? argv[1] : "once";
    FastServer server;
    check(server.port() != 0, "the loopback server started");

    check(egress::egress_context_constructions() == 0, "no context exists before the first call");

    if (mode == "once") {
        // G13b: 20 gate-rejected calls never touch the context.
        TestEgressProxy p;
        configure(p);
        for (int i = 0; i < 20; ++i) {
            auto r = p.fetch_blocking(NetEgressRequest{"GET", "/", {}, "body on a GET"}, grant_to(server.port()),
                                      EgressCallControl{});
            check(!r.has_value() && r.error().code == "net.invalid_request", "G13b a gate-rejected call is refused");
        }
        for (int i = 0; i < 20; ++i) {
            auto r = p.fetch_blocking(NetEgressRequest{"GET", "/", {}, ""}, NetOut{}, EgressCallControl{});
            check(!r.has_value() && r.error().code == "net.ambiguous_grant", "G13b a gate-rejected call (grant) is refused");
        }
        check(egress::egress_context_constructions() == 0, "G13b 40 gate-rejected calls constructed NO I/O context");

        // G13d: 64 concurrent FIRST calls create exactly one context.
        // 16 threads, not 64: the test caps the data segment (memory_cap, 512 MiB) and on Linux every thread's default
        // 8 MiB mmap'd stack is charged against RLIMIT_DATA, so 64 test threads plus the egress context's own threads
        // exhaust the cap (std::system_error, EAGAIN). Windows does not charge stacks that way. A failed thread
        // creation fails the row cleanly instead of terminating.
        constexpr int kThreads = 16;
        std::latch start(kThreads + 1);
        std::atomic<int> ok{0};
        int created = 0;
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < kThreads; ++i) {
                try {
                    threads.emplace_back([&] {
                        TestEgressProxy q;
                        configure(q);
                        start.arrive_and_wait();
                        auto r = q.fetch_blocking(NetEgressRequest{"GET", "/", {}, ""}, grant_to(server.port()), EgressCallControl{});
                        if (r.has_value() && r->status == 200) ok.fetch_add(1);
                    });
                    ++created;
                } catch (std::system_error const& e) {
                    check(false, std::string("G13d could not create thread ") + std::to_string(i) + ": " + e.what());
                    start.count_down(kThreads + 1 - created);  // release the slots of threads that will never arrive
                    break;
                }
            }
            if (created == kThreads) start.arrive_and_wait();
        }
        check(egress::egress_context_constructions() == 1, "G13d " + std::to_string(kThreads) + " concurrent first calls created exactly ONE context (" +
                                                               std::to_string(egress::egress_context_constructions()) + ")");
        check(ok.load() > 0, "G13d the concurrent first calls succeeded (" + std::to_string(ok.load()) + "/" + std::to_string(kThreads) + "; the rest may be egress_busy)");
    } else {
        // G13f: a construction failure is reported, and the NEXT call retries.
        egress::egress_inject_context_failures(1);
        TestEgressProxy p;
        configure(p);
        auto first = p.fetch_blocking(NetEgressRequest{"GET", "/", {}, ""}, grant_to(server.port()), EgressCallControl{});
        check(!first.has_value() && first.error().code == "net.egress_unavailable" && first.error().klass == failure_class::resource,
              "G13f a construction failure is net.egress_unavailable / resource");
        check(egress::egress_context_constructions() == 0, "G13f nothing was constructed by the failed attempt");
        auto second = p.fetch_blocking(NetEgressRequest{"GET", "/", {}, ""}, grant_to(server.port()), EgressCallControl{});
        check(second.has_value() && second->status == 200, "G13f the next call retries and succeeds");
        check(egress::egress_context_constructions() == 1, "G13f exactly one context exists after the retry");
        // The async entry reports the same failure the same way.
        egress::egress_inject_context_failures(0);
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_net_egress_first_use (%s): ALL PASS\n", mode.c_str());
        return 0;
    }
    std::fprintf(stderr, "test_net_egress_first_use (%s): %d FAILURE(S)\n", mode.c_str(), g_failures);
    return 1;
}
