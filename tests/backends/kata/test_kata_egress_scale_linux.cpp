// Scaling check for the shared-table egress policy (ADR-239, red-team B2): throughput of one policed guest
// and the control-plane cost of one more sandbox, with 1, 50, 100 and N real bridge ports each carrying a full
// per-sandbox policy (HostEgress::install: port flags, tc, chains, quota, vmap entries). THE GUESTS HERE ARE
// netns+veth LAB GUESTS, not microVMs: the datapath cost is the same (frames cross the veth host end and the
// bridge hooks) but boot cost and VM behaviour are out of scope. N comes from AEPV_SCALE_N (default 200).
//
// Positive control: the layout the design first proposed (one nft table per sandbox, each with its own hook
// chains) is built at 100 sandboxes and measured by the same probe; if that did not slow the guest down, the
// probe could not see an O(N) layout and the shared-table result would mean nothing.
// REQUIRES: root, python3, nft/tc/bridge/ip, operator FORWARD accepts for aepvbr0 <-> aepvw0 (ae-prove-temp).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "backends/kata/egress_policy.hpp"
#include "egress_test_support.hpp"

using namespace aepv;

namespace {

constexpr char kBr[] = "aepvbr0";

std::string upload_script(long long bytes, std::string const& tag) {
    std::ostringstream o;
    o << "import socket, time\nN=" << bytes << "\ns=socket.socket(); s.settimeout(30); s.connect(('198.51.100.2',9000)); s.sendall(b'UP " << tag << "\\n')\n"
      << "chunk=b'u'*1048576; sent=0; t0=time.time()\n"
         "while sent<N:\n    m=min(len(chunk),N-sent); s.sendall(chunk[:m]); sent+=m\n"
         "s.shutdown(socket.SHUT_WR); r=s.recv(64); dt=time.time()-t0\n"
         "print('GBIT %.2f' % (sent*8/dt/1e9), r.decode().strip())\n";
    return o.str();
}

double gbit(std::string const& out) {
    std::size_t at = out.find("GBIT ");
    return at == std::string::npos ? -1.0 : std::atof(out.c_str() + at + 5);
}

double median_run(std::string const& tag, int runs = 3) {
    std::vector<double> v;
    for (int i = 0; i < runs; ++i) {
        Sh r = sh("ip netns exec aepv-g1 python3 -c " + shq(upload_script(1073741824LL, tag + std::to_string(i))), 120);
        v.push_back(gbit(r.out));
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

egress::SandboxPlan plan_for(int i) {
    egress::SandboxPlan p;
    char mac[32];
    std::snprintf(mac, sizeof mac, "02:aa:00:%02x:%02x:%02x", (i >> 16) & 0xff, (i >> 8) & 0xff, i & 0xff);
    p.tag = "aepv-sc" + std::to_string(i);
    p.veth = i == 1 ? "aepvg1h" : "aepvs" + std::to_string(i);
    p.bridge = kBr;
    p.guest_mac = mac;
    p.guest_ip = (10u << 24) | (99u << 16) | (static_cast<unsigned>(i / 240) << 8) | static_cast<unsigned>(10 + i % 240);
    p.gateway_ip = (10u << 24) | (99u << 16) | 1u;
    p.allowed = {{(198u << 24) | (51u << 16) | (100u << 8) | 2u, 9000}, {(93u << 24) | (184u << 16) | (216u << 8) | 34u, 443}};
    p.net_bytes = 0;
    return p;
}

// The first design's layout: one table per sandbox, each with its own input/forward/output hook chains.
std::string naive_table(int i, std::string const& veth, std::string const& mac, std::string const& ip) {
    std::ostringstream o;
    o << "table bridge aepv_naive" << i << " {\n";
    for (char const* hook : {"input", "forward", "output"}) {
        o << " chain c_" << hook << " { type filter hook " << hook << " priority -300; policy accept;\n";
        if (std::string(hook) == "output") {
            o << "  oifname \"" << veth << "\" ether type arp accept\n";
            o << "  oifname \"" << veth << "\" ip saddr 198.51.100.2 ip daddr " << ip << " tcp sport 9000 ct state established,related accept\n";
            o << "  oifname \"" << veth << "\" ip saddr 93.184.216.34 ip daddr " << ip << " tcp sport 443 ct state established,related accept\n";
            o << "  oifname \"" << veth << "\" counter drop\n";
        } else {
            o << "  iifname \"" << veth << "\" ether saddr != " << mac << " drop\n";
            o << "  iifname \"" << veth << "\" ether type arp accept\n";
            o << "  iifname \"" << veth << "\" ip saddr " << ip << " ip daddr 198.51.100.2 tcp dport 9000 accept\n";
            o << "  iifname \"" << veth << "\" ip saddr " << ip << " ip daddr 93.184.216.34 tcp dport 443 accept\n";
            o << "  iifname \"" << veth << "\" counter drop\n";
        }
        o << " }\n";
    }
    o << "}\n";
    return o.str();
}

}  // namespace

int main() {
    if (::geteuid() != 0) {
        std::fprintf(stderr, "FAIL: needs root\n");
        return 1;
    }
    int const N = std::getenv("AEPV_SCALE_N") != nullptr ? std::atoi(std::getenv("AEPV_SCALE_N")) : 200;
    Lab lab;
    check(lab.up(), "lab up (world netns + sinks)");
    check(Lab::forward_precondition_met(), "PRECONDITION (operator): FORWARD accepts for aepvbr0 <-> aepvw0 (ae-prove-temp)");
    if (g_failures != 0) return 1;
    egress::HostEgress he(real_runner());

    // lab bridge and the measured guest
    for (char const* c : {"ip link add aepvbr0 type bridge", "ip addr add 10.99.0.1/16 dev aepvbr0", "ip link set aepvbr0 up", "ip netns add aepv-g1",
                          "ip link add aepvg1h type veth peer name aepvg1p", "ip link set aepvg1p netns aepv-g1 name eth0", "ip netns exec aepv-g1 ip link set lo up",
                          "ip netns exec aepv-g1 ip link set eth0 address 02:aa:00:00:00:01", "ip netns exec aepv-g1 ip addr add 10.99.0.11/16 dev eth0",
                          "ip netns exec aepv-g1 ip link set eth0 up", "ip netns exec aepv-g1 ip route add default via 10.99.0.1", "ip link set aepvg1h master aepvbr0",
                          "ip link set aepvg1h up", "sysctl -qw net.ipv4.conf.aepvbr0.rp_filter=0"}) {
        check(sh(c).ok(), std::string("lab: ") + c);
    }
    check(sh("ip netns exec aepv-g1 python3 -c \"import socket; print(1)\"").ok(), "lab guest runs python");

    // control 0: no policy at all (the unpoliced bridge) -- also proves the lab path works
    double const unpoliced = median_run("u");
    check(unpoliced > 0.5, "lab path works without policy: " + std::to_string(unpoliced) + " Gbit/s");

    // N ports, each with a full policy
    std::vector<int> const checkpoints{1, 50, 100, N};
    std::vector<std::pair<int, double>> results;
    std::vector<double> install_ms;
    {
        std::string batch;
        for (int i = 2; i <= N; ++i) {
            batch += "link add aepvs" + std::to_string(i) + " type veth peer name aepvt" + std::to_string(i) + "\n";
            batch += "link set aepvs" + std::to_string(i) + " master " + kBr + " up\nlink set aepvt" + std::to_string(i) + " up\n";
        }
        std::ofstream("/tmp/aepv-scale.batch") << batch;
        check(sh("ip -batch /tmp/aepv-scale.batch", 300).ok(), "lab: " + std::to_string(N - 1) + " extra bridge ports created");
    }
    int installed = 0;
    for (int i = 1; i <= N; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        auto r = he.install(plan_for(i));
        double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!r) {
            check(false, "install #" + std::to_string(i) + " failed: " + r.error().message);
            break;
        }
        ++installed;
        install_ms.push_back(ms);
        if (std::find(checkpoints.begin(), checkpoints.end(), i) != checkpoints.end()) {
            double const g = median_run("p" + std::to_string(i) + "-");
            results.emplace_back(i, g);
            std::fprintf(stderr, "scale: N=%d shared-table throughput %.2f Gbit/s (install #%d took %.0f ms)\n", i, g, i, ms);
        }
    }
    check(installed == N, "all " + std::to_string(N) + " sandbox policies installed");
    double base = results.empty() ? 0 : results.front().second;
    for (auto const& [n, g] : results) {
        check(g >= 0.8 * base && g > 0, "shared table: throughput at N=" + std::to_string(n) + " (" + std::to_string(g) + " Gbit/s) >= 80% of N=1 (" + std::to_string(base) + ")");
    }
    // control-plane
    {
        auto t0 = std::chrono::steady_clock::now();
        Sh l = sh("nft list table bridge ae_k > /dev/null");
        double const list_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        egress::SandboxPlan extra = plan_for(N + 1);
        extra.veth = "aepvx1";
        (void)sh("ip link add aepvx1 type veth peer name aepvx2 && ip link set aepvx1 master aepvbr0 up && ip link set aepvx2 up");
        t0 = std::chrono::steady_clock::now();
        auto r = he.install(extra);
        double const add_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        (void)sh("ip link delete aepvx1");
        t0 = std::chrono::steady_clock::now();
        auto rm = he.remove(extra.tag, extra.veth);
        double const rm_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(r.has_value() && rm.has_value() && l.ok(), "control plane at N=" + std::to_string(N) + ": add, remove, list all succeed");
        std::fprintf(stderr, "scale: control plane at N=%d: install one more sandbox %.0f ms, remove one %.0f ms, `nft list table` %.0f ms\n", N, add_ms, rm_ms, list_ms);
        evidence("scaling: control-plane cost at N=" + std::to_string(N) + " ports", "per-sandbox-table layout (red-team measurement): add 225 ms / delete 231 ms / list 220 ms at N=600",
                 "add " + std::to_string(static_cast<int>(add_ms)) + " ms / remove " + std::to_string(static_cast<int>(rm_ms)) + " ms / list " + std::to_string(static_cast<int>(list_ms)) + " ms");
    }

    // POSITIVE CONTROL: the per-sandbox-table layout at 100 sandboxes, same probe, policy tables of the shared design removed
    std::string shared_out = sh("nft list table bridge ae_k").out;
    (void)shared_out;
    (void)sh("nft delete table bridge ae_k");  // measure the naive layout alone (the shared table is rebuilt by the sweep/installs after)
    {
        for (int i = 2; i <= 100; ++i) {
            std::ofstream("/tmp/aepv-naive.nft") << naive_table(i, "aepvs" + std::to_string(i), "02:aa:00:00:00:99", "10.99.1." + std::to_string(i % 250));
            if (!sh("nft -f /tmp/aepv-naive.nft").ok()) {
                check(false, "naive table " + std::to_string(i) + " loads");
                break;
            }
        }
        std::ofstream("/tmp/aepv-naive.nft") << naive_table(1, "aepvg1h", "02:aa:00:00:00:01", "10.99.0.11");
        (void)sh("nft -f /tmp/aepv-naive.nft");
        double const naive = median_run("n");
        std::fprintf(stderr, "scale: CONTROL per-sandbox-table layout at 100 sandboxes: %.2f Gbit/s (unpoliced %.2f)\n", naive, unpoliced);
        check(naive > 0 && naive < 0.8 * base, "CONTROL: the per-sandbox-table layout at 100 sandboxes IS slower (" + std::to_string(naive) + " Gbit/s < 80% of " + std::to_string(base) + ") -- the probe can see an O(N) layout");
        for (int i = 1; i <= 100; ++i) (void)sh("nft delete table bridge aepv_naive" + std::to_string(i));
        std::string line;
        for (auto const& [n, g] : results) line += "N=" + std::to_string(n) + ": " + std::to_string(g).substr(0, 5) + " Gbit/s; ";
        evidence("scaling: shared-table guest throughput stays flat with N sandboxes (lab netns guests)", "per-sandbox-table layout at N=100: " + std::to_string(naive).substr(0, 5) + " Gbit/s; unpoliced " + std::to_string(unpoliced).substr(0, 5),
                 line);
    }

    // teardown of the lab
    (void)sh("ip netns delete aepv-g1");
    for (int i = 1; i <= N; ++i) (void)sh("ip link delete " + std::string(i == 1 ? "aepvg1h" : "aepvs" + std::to_string(i)) + " 2>/dev/null");
    (void)he.sweep_orphans();
    (void)sh("ip link delete aepvbr0");
    (void)sh("rm -f /tmp/aepv-scale.batch /tmp/aepv-naive.nft");
    check(ae_tables() == "0", "no ae_k table left after the sweep");
    lab.down();
    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_egress_scale_linux: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_egress_scale_linux: %d FAILURE(S)\n", g_failures.load());
    return 1;
}
