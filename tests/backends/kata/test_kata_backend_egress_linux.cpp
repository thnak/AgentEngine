// Acceptance tests for the KataBackend egress policy (ADR-240, GitHub #194): the allowlist, the L2/L3
// containment of a hostile guest, and the net_bytes quota, each with a POSITIVE CONTROL (a hand-wired sandbox
// with no policy, i.e. the pre-change behaviour, or the same probe with the barrier removed) so no check can pass
// vacuously. See egress_test_support.hpp for the lab, the preconditions and what the runbook wrapper provides.
// REQUIRES: root, containerd + Kata (kata-clh), CNI `ae-kata-net`, cnitool, nft/tc/bridge/ip, python3, the
// python:3.12-alpine image pulled, FORWARD accepts for aekata0 <-> aepvw0 (comment ae-prove-temp).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/kata/kata_backend.hpp"
#include "egress_attack_kit.hpp"

using namespace agentengine;
using namespace agentengine::kata;
using namespace aepv;

namespace {

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.trace_id = "kata-backend-egress-test";
    ctx.span_id = "span-1";
    return ctx;
}

SandboxSpec make_spec(std::vector<std::string> const& entries, std::uint64_t net_bytes = 0) {
    SandboxSpec s;
    for (auto const& e : entries) s.net.allowlist.push_back(e);
    s.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxNetOut{entries}}});
    s.limits.net_bytes = net_bytes;
    s.limits.wall_ms = 90000;
    return s;
}

struct Box {
    SandboxHandle handle;
    GuestFacts facts;
    [[nodiscard]] std::string const& id() const { return handle.opaque_id; }
};

std::optional<Box> make_box(KataBackend& be, SandboxSpec const& spec, EffectContext& ctx, std::string const& label) {
    auto h = agentengine::rt::block_on(be.create(spec, ctx));
    check(h.has_value(), "create(): " + label + (h ? "" : " -- " + h.error().message));
    if (!h) return std::nullopt;
    Box b{*h, {}};
    b.facts = guest_facts(b.id());
    check(!b.facts.mac.empty() && !b.facts.ip.empty(), label + ": guest has an eth0 address (" + b.facts.ip + " " + b.facts.mac + ")");
    return b;
}

std::string bexec(KataBackend& be, Box& b, std::string const& source, EffectContext& ctx) {
    ExecRequest r;
    r.source = source;
    auto o = agentengine::rt::block_on(be.exec(b.handle, r, ctx));
    if (!o) return "EXEC-ERROR " + o.error().code + ": " + o.error().message;
    return o->stdout_text;
}

std::string bpy(KataBackend& be, Box& b, std::string const& script, EffectContext& ctx) {
    return bexec(be, b, "python3 -c " + shq(script), ctx);
}

void destroy_box(KataBackend& be, Box& b) { agentengine::rt::block_on(be.destroy(b.handle)); }

constexpr char kListener[] =
    "import socket\ns=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', 7777)); s.listen(8)\n"
    "while True:\n    c,_=s.accept(); c.sendall(b'PEER-OK\\n'); c.close()\n";

void start_listener(std::string const& id) { (void)guest_exec(id, "(nohup python3 -c " + shq(kListener) + " >/dev/null 2>&1 &) ; sleep 1"); }

long long frag_memory() { return first_int_after(sh("grep '^FRAG' /proc/net/sockstat").out, "memory"); }

void wait_frag_drain() {
    for (int i = 0; i < 50 && frag_memory() > 100000; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
}

std::string flood_ct_script(AttackTarget const& t) {
    std::ostringstream o;
    o << "MYMAC='" << t.attacker_mac << "'\nMYIP='" << t.attacker_ip << "'\nGWMAC='" << t.gateway_mac << "'\nW='" << t.world_ip << "'\n" << kGuestLib
      << "for i in range(20000):\n    RAW.send(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 6, tcp(MYIP, W, 1024 + (i % 60000), 9000, b'AEPV-FLOOD', 0x02), ident=i & 0xffff)))\nprint('FLOOD-DONE')\n";
    return o.str();
}

std::string flood_frag_script(AttackTarget const& t) {
    std::ostringstream o;
    o << "MYMAC='" << t.attacker_mac << "'\nMYIP='" << t.attacker_ip << "'\nGWMAC='" << t.gateway_mac << "'\nW='" << t.world_ip << "'\n" << kGuestLib
      << "pl = tcp(MYIP, W, 40000, 9000, b'F' * 1436)\n"
         "for i in range(20000):\n    RAW.send(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 6, pl, ident=i & 0xffff, mf=1, off=0)))\nprint('FLOOD-DONE')\n";
    return o.str();
}

std::string upload_script(std::string const& ip, long long n, std::string const& tag) {
    std::ostringstream o;
    o << "import socket\nIP='" << ip << "'; N=" << n << "; TAG='" << tag << "'\n"
      << "s=socket.socket(); s.settimeout(10); s.connect((IP,9000)); s.sendall(('UP %s\\n'%TAG).encode())\n"
         "sent=0; chunk=b'u'*65536\n"
         "try:\n"
         "    while sent<N:\n"
         "        m=min(65536,N-sent); s.sendall(chunk[:m]); sent+=m\n"
         "except Exception as e:\n"
         "    pass\n"
         "print('SENT',sent)\n"
         "try:\n"
         "    s.shutdown(socket.SHUT_WR); s.settimeout(10); print('REPLY',s.recv(64))\n"
         "except Exception as e:\n"
         "    print('REPLY none')\n";
    return o.str();
}

std::string download_script(std::string const& ip, long long n) {
    std::ostringstream o;
    o << "import socket\nIP='" << ip << "'; N=" << n << "\n"
      << "s=socket.socket(); s.settimeout(8); s.connect((IP,9000)); s.sendall(('DOWN %d\\n'%N).encode())\n"
         "got=0\n"
         "try:\n"
         "    while True:\n"
         "        d=s.recv(65536)\n"
         "        if not d: break\n"
         "        got+=len(d)\n"
         "except Exception as e:\n"
         "    pass\n"
         "print('RECV',got)\n";
    return o.str();
}

long long sink_total(std::string const& tag) {
    // the sink logs one 'UP <tag> <total>' line per upload; sum them (a sandbox uploads once per tag)
    Sh r = sh("grep '^UP " + tag + " ' /tmp/aepv-sink.log | tail -1");
    std::size_t at = r.out.rfind(' ');
    return at == std::string::npos ? -1 : std::atoll(r.out.c_str() + at + 1);
}

long long quota_used(std::string const& tag) {
    Sh r = sh("nft -j list quota bridge ae_k q_" + tag);
    return first_int_after(r.out, "\"used\":");
}

std::string world_connect_to_guest(std::string const& guest_ip) {
    // the world dials the guest from source port 9000, i.e. exactly the return-path tuple of an allowed flow
    std::string const py =
        "import socket\ns=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1); s.bind(('198.51.100.2', 9000)); s.settimeout(5)\n"
        "try:\n    s.connect(('" + guest_ip + "', 7777)); print('INBOUND-CONNECTED', s.recv(32))\nexcept Exception as e:\n    print('INBOUND-FAILED', type(e).__name__)\n";
    return sh("ip netns exec aepv-w python3 -c " + shq(py), 30).out;
}

// Number of host conntrack entries whose ORIGINAL direction source is `ip` (the guest), so ageing entries of
// earlier runs do not blur the delta.
// (/proc/net/nf_conntrack is not exposed on this kernel, so the flood is measured at the world side instead:
// every SYN that reaches the world veth is a forwarded packet that created a host conntrack entry; the global
// nf_conntrack_count delta is reported alongside as supporting data only, it ages concurrently.)
struct FloodResult {
    int forwarded = -1;
    long long ct_delta = 0;
};
FloodResult run_ct_flood(std::string const& id, AttackTarget const& t) {
    long long const c0 = first_int_after(sh("cat /proc/sys/net/netfilter/nf_conntrack_count").out, "");
    auto sn = Lab::sniff_start("aepvw0", 12, {"AEPV-FLOOD"});
    (void)guest_py(id, flood_ct_script(t), 80);
    auto m = Lab::sniff_collect(sn);
    FloodResult r;
    r.forwarded = m.count("AEPV-FLOOD") != 0 ? m["AEPV-FLOOD"] : -1;
    r.ct_delta = first_int_after(sh("cat /proc/sys/net/netfilter/nf_conntrack_count").out, "") - c0;
    return r;
}

// Hammers spoofed-source-MAC frames from `attacker_id` for ~6 s while the host polls where the victim's MAC
// is learned. Learning runs before netfilter, so a guest that is merely filtered still poisons the FDB (R10).
std::string fdb_poison_probe(std::string const& attacker_id, AttackTarget const& t) {
    std::ostringstream o;
    o << "MYMAC='" << t.attacker_mac << "'\nVMAC='" << t.victim_mac << "'\nGWMAC='" << t.gateway_mac << "'\n" << kGuestLib
      << "end = time.time() + 6\nwhile time.time() < end:\n    snd(eth(GWMAC, VMAC, 0x88b5, b'AEPV-FDBP'), 3)\n    time.sleep(0.05)\nprint('FDB-DONE')\n";
    std::string const script = o.str();
    std::thread guest([&] { (void)guest_py(attacker_id, script, 60); });
    std::string seen;
    for (int i = 0; i < 11; ++i) {
        std::string const dev = fdb_dev_of(t.victim_mac);
        if (seen.find(dev) == std::string::npos) seen += dev + " ";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    guest.join();
    return seen;
}

std::string firewall_snapshot() {
    // chain headers of iptables-save carry packet/byte counters ([pkts:bytes]); everything else is rule text
    return sh("iptables-save | grep -v '^#' | sed 's/\\[[0-9]*:[0-9]*\\]/[]/' ; nft -s list ruleset | grep -v ' ae_k'", 60).out;
}

}  // namespace

int main() {
    EffectContext ctx = make_ctx();
    if (::geteuid() != 0) {
        std::fprintf(stderr, "FAIL: these tests need root (netns, nft, tc, ctr)\n");
        return 1;
    }
    Lab lab;
    check(lab.up(), "lab: world netns aepv-w + sinks up");
    if (g_failures != 0) return 1;
    check(Lab::forward_precondition_met(),
          "PRECONDITION (operator, not the backend): FORWARD accepts for aekata0 <-> aepvw0 with comment ae-prove-temp exist");
    check(ae_tables() == "0", "no ae_k table exists before the run");
    std::string const fw_before = firewall_snapshot();
    std::string const ports_before = bridge_ports();
    std::string const netns_before = netns_list();
    std::string const containers_before = ctr_containers();

    std::string const lan_ip = trim(sh("ip -4 route get 1.1.1.1 | sed -n 's/.* src \\([0-9.]*\\).*/\\1/p' | head -1").out);
    std::string const v6ll = trim(sh("ip -6 -o addr show dev aekata0 scope link | sed -n 's/.*inet6 \\(fe80[0-9a-f:]*\\)\\/.*/\\1/p' | head -1").out);
    std::string const gw_mac = host_mac("aekata0");
    check(!lan_ip.empty() && !v6ll.empty() && !gw_mac.empty(), "host facts: lan=" + lan_ip + " bridge v6=" + v6ll + " bridge mac=" + gw_mac);

    // ======================= PHASE A: controls (hand-wired sandboxes, NO policy) =======================
    std::map<std::string, std::string> ctl;
    std::map<std::string, int> ctl_attack;
    std::string ctl_fdb_dev, ctl_neigh;
    long long ctl_ct_delta = -1, ctl_ct_ct = 0, ctl_frag_peak = -1;
    std::string ctl_inbound, c1_veth, c2_veth;
    {
        ControlSandbox c1, c2;
        bool const up = c1.create("aepv-c1") && c2.create("aepv-c2");
        check(up, "control sandboxes aepv-c1/aepv-c2 boot (hand-wired netns + cnitool + ctr run, no policy)");
        if (up) {
            c1_veth = c1.veth;
            c2_veth = c2.veth;
            start_listener(c2.id);
            (void)guest_py(c2.id, probe_script({{"gw", "10.88.77.1", "22"}}));  // c2 transmits: its MAC is learned
            Sh r = guest_py(c1.id, probe_script({{"w9000", "198.51.100.2", "9000"}, {"w9001", "198.51.100.2", "9001"}, {"w2_9000", "198.51.100.3", "9000"},
                                                  {"gw22", "10.88.77.1", "22"}, {"lan22", lan_ip, "22"}, {"v6", v6ll, "22"}, {"c2", c2.ip, "7777"}}));
            for (char const* l : {"w9000", "w9001", "w2_9000", "gw22", "lan22", "v6", "c2"}) ctl[l] = parse_probe(r.out, l);
            check(ctl["w9000"].find("WORLD-OK") != std::string::npos, "CONTROL: unpoliced guest reaches the world sink 198.51.100.2:9000 (" + ctl["w9000"] + ")");
            check(ctl["w9001"].find("WORLD-OK") != std::string::npos, "CONTROL: unpoliced guest reaches the same IP on port 9001");
            check(ctl["w2_9000"].find("WORLD-OK") != std::string::npos, "CONTROL: unpoliced guest reaches 198.51.100.3:9000");
            check(ctl["gw22"].find("SSH-") != std::string::npos, "CONTROL: unpoliced guest reads the host sshd banner on the bridge gateway (" + ctl["gw22"] + ")");
            check(ctl["lan22"].find("SSH-") != std::string::npos, "CONTROL: unpoliced guest reads the host sshd banner on the host LAN IP " + lan_ip);
            check(ctl["v6"].find("SSH-") != std::string::npos, "CONTROL: unpoliced guest reaches the host over IPv6 link-local");
            check(ctl["c2"].find("PEER-OK") != std::string::npos, "CONTROL: unpoliced guest reaches another guest on the bridge");

            // attack batch against the unpoliced pair: every marker class that can reach is expected to
            AttackTarget t{c1.mac, c1.ip, c2.mac, c2.ip, gw_mac};
            ctl_attack = run_attack_batch(c1.id, t);
            std::fprintf(stderr, "control attack counts: %s\n", counts_line(ctl_attack).c_str());
            for (auto const& m : legit_markers()) check(ctl_attack[m] > 0, "CONTROL: sniffers + sender work (" + m + " seen)");
            ctl_neigh = sh("ip neigh show " + c2.ip + " dev aekata0").out;
            ctl_fdb_dev = fdb_poison_probe(c1.id, t);
            check(ctl_fdb_dev.find(c1.veth) != std::string::npos, "CONTROL: spoofed source MAC poisons the FDB (victim mac was seen behind: " + ctl_fdb_dev + "; attacker port " + c1.veth + ")");
            check(ctl_neigh.find(c1.mac) != std::string::npos, "CONTROL: forged ARP poisons the host neighbour table (" + trim(ctl_neigh) + ")");

            FloodResult const cf = run_ct_flood(c1.id, t);
            ctl_ct_delta = cf.forwarded;
            ctl_ct_ct = cf.ct_delta;
            check(ctl_ct_delta > 10000, "CONTROL: an unpoliced guest gets >10000 of 20000 flood SYNs forwarded to the world (" + std::to_string(ctl_ct_delta) + "; global conntrack +" + std::to_string(ctl_ct_ct) + ")");

            wait_frag_drain();
            (void)guest_py(c1.id, flood_frag_script(t), 80);
            ctl_frag_peak = frag_memory();
            check(ctl_frag_peak > 1000000, "CONTROL: an unpoliced guest pins >1 MB of host IP fragment memory (" + std::to_string(ctl_frag_peak) + ")");

            start_listener(c1.id);
            ctl_inbound = world_connect_to_guest(c1.ip);
            check(ctl_inbound.find("INBOUND-CONNECTED") != std::string::npos, "CONTROL: the world can dial an unpoliced guest's listener from source port 9000 (" + trim(ctl_inbound) + ")");
        }
        c1.destroy();
        c2.destroy();
    }
    wait_frag_drain();
    check(ae_tables() == "0", "controls left no ae_k table (the backend never ran)");

    // ======================= PHASE B: the same probes against policy sandboxes ===========================
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s1 = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "S1 (allows 198.51.100.2:9000)");
        auto s2 = make_box(backend, make_spec({"198.51.100.3:9000:http"}), ctx, "S2 (allows 198.51.100.3:9000)");
        if (s1 && s2) {
            start_listener(s2->id());
            // table facts while sandboxes exist
            check(ae_tables() == "1", "exactly ONE shared ae_k table exists with two sandboxes (not one per sandbox)");
            std::string const s2_veth_before = fdb_dev_of(s2->facts.mac);
            check(s2_veth_before != "NONE" && s2_veth_before != fdb_dev_of(s1->facts.mac), "each guest MAC has its own static FDB port (" + s2_veth_before + ")");

            std::string out = bpy(backend, *s1, probe_script({{"w9000", "198.51.100.2", "9000"}, {"w9001", "198.51.100.2", "9001"}, {"w2_9000", "198.51.100.3", "9000"},
                                                               {"gw22", "10.88.77.1", "22"}, {"lan22", lan_ip, "22"}, {"v6", v6ll, "22"}, {"s2", s2->facts.ip, "7777"}}), ctx);
            auto const p = [&](char const* l) { return parse_probe(out, l); };
            check(p("w9000").find("WORLD-OK 198.51.100.2:9000") != std::string::npos, "ALLOW: the guest reaches exactly the allowlisted (ip,port) (" + p("w9000") + ")");
            check(p("w9001").rfind("FAILED", 0) == 0, "DENY: same allowed IP, other port 9001, is blocked (" + p("w9001") + ")");
            check(p("w2_9000").rfind("FAILED", 0) == 0, "DENY: an unlisted IP (198.51.100.3:9000) is blocked (" + p("w2_9000") + ")");
            check(p("gw22").find("SSH-") == std::string::npos && p("gw22").rfind("FAILED", 0) == 0, "DENY: host sshd on the bridge gateway is blocked (" + p("gw22") + ")");
            check(p("lan22").find("SSH-") == std::string::npos && p("lan22").rfind("FAILED", 0) == 0, "DENY: host sshd on the LAN IP is blocked (" + p("lan22") + ")");
            check(p("v6").find("SSH-") == std::string::npos && p("v6").rfind("FAILED", 0) == 0, "DENY: IPv6 link-local to the host is blocked (" + p("v6") + ")");
            check(p("s2").rfind("FAILED", 0) == 0, "DENY: guest-to-guest (S1 -> S2:7777) is blocked (" + p("s2") + ")");
            evidence("allowlisted (ip,port) reachable; same IP other port, unlisted IP blocked", "control reaches all three",
                     p("w9000") + " | " + p("w9001") + " | " + p("w2_9000"));
            evidence("host sshd (gateway + LAN IP), IPv6 link-local, guest-to-guest blocked", "control reads SSH banner x2, v6 banner, peer",
                     p("gw22") + " | " + p("lan22") + " | " + p("v6") + " | " + p("s2"));

            // S2's own listener is reachable by S2's own policy-free loopback only; confirm S2 is alive and policed too
            std::string const s2out = bpy(backend, *s2, probe_script({{"w3", "198.51.100.3", "9000"}, {"w2", "198.51.100.2", "9000"}}), ctx);
            check(parse_probe(s2out, "w3").find("WORLD-OK") != std::string::npos && parse_probe(s2out, "w2").rfind("FAILED", 0) == 0,
                  "ALLOW/DENY holds per sandbox: S2 reaches only its own tuple (198.51.100.3:9000)");

            // the hostile batch
            AttackTarget t{s1->facts.mac, s1->facts.ip, s2->facts.mac, s2->facts.ip, gw_mac};
            auto counts = run_attack_batch(s1->id(), t);
            std::fprintf(stderr, "policy attack counts: %s\n", counts_line(counts).c_str());
            for (auto const& m : legit_markers()) check(counts[m] > 0, "positive control under policy: legitimate " + m + " still gets through (sender and sniffers work)");
            int leaked = 0;
            std::string leaked_names;
            for (auto const& m : blocked_markers()) {
                if (counts[m] != 0) {
                    ++leaked;
                    leaked_names += m + " ";
                }
            }
            check(leaked == 0, "every hostile frame class is blocked (fragments, VLAN, UDP, spoofed IP/MAC, ICMP, IPIP, GRE, IPv6, raw ethertype, forged ARP, FDB spoof); leaked: " + leaked_names);
            std::string ctl_seen;
            for (auto const& m : blocked_markers()) {
                if (ctl_attack[m] > 0) ctl_seen += m.substr(5) + " ";
            }
            evidence("hostile frame matrix blocked: " + counts_line(counts), "unpoliced control delivered classes: " + ctl_seen + "(" + counts_line(ctl_attack) + ")", leaked == 0 ? "held" : "FAILED");
            std::string const s1_veth = fdb_dev_of(s1->facts.mac);
            std::string const fdb_seen = fdb_poison_probe(s1->id(), t);
            check(fdb_seen == s2_veth_before + " ", "FDB poisoning blocked: the victim MAC was only ever behind its own port (" + fdb_seen + "; attacker port " + s1_veth + "; control saw: " + ctl_fdb_dev + ")");
            std::string const s2_veth_after = fdb_seen;
            std::string const neigh = sh("ip neigh show " + s2->facts.ip + " dev aekata0").out;
            check(neigh.find(s1->facts.mac) == std::string::npos, "forged ARP did not poison the host neighbour table (" + trim(neigh) + "; control: " + trim(ctl_neigh) + ")");
            evidence("spoofed source MAC does not poison another sandbox's FDB entry / forged ARP does not poison host neighbours",
                     "control: victim MAC seen behind {" + ctl_fdb_dev + "}, neigh " + trim(ctl_neigh), "FDB {" + s2_veth_after + "} | " + trim(neigh));

            // conntrack and fragment-memory flooding
            FloodResult const pf = run_ct_flood(s1->id(), t);
            long long const ct_delta = pf.forwarded;
            check(ct_delta >= 0 && ct_delta < 1500, "conntrack flood contained by the SYN rate limit: of 20000 SYNs " + std::to_string(ct_delta) + " were forwarded (control: " + std::to_string(ctl_ct_delta) + "; global conntrack +" + std::to_string(pf.ct_delta) + ")");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::string const after_flood = bpy(backend, *s1, probe_script({{"w9000", "198.51.100.2", "9000"}}), ctx);
            check(parse_probe(after_flood, "w9000").find("WORLD-OK") != std::string::npos, "legitimate connect still works after the SYN flood (rate limit is a bucket, not a ban)");
            evidence("conntrack flood bounded (SYN rate limit)", "control: " + std::to_string(ctl_ct_delta) + " of 20000 SYNs forwarded (conntrack +" + std::to_string(ctl_ct_ct) + ")",
                     std::to_string(ct_delta) + " of 20000 forwarded (conntrack +" + std::to_string(pf.ct_delta) + ")");
            wait_frag_drain();
            (void)guest_py(s1->id(), flood_frag_script(t), 80);
            long long const frag_peak = frag_memory();
            check(frag_peak < 200000, "IP fragment flood does not pin host reassembly memory: " + std::to_string(frag_peak) + " bytes (control peak " + std::to_string(ctl_frag_peak) + ")");
            evidence("fragment-memory flood bounded (prerouting -450 + tc nofrag)", "control peak " + std::to_string(ctl_frag_peak) + " bytes", std::to_string(frag_peak) + " bytes");

            // unsolicited inbound
            start_listener(s1->id());
            std::string const inbound = world_connect_to_guest(s1->facts.ip);
            check(inbound.find("INBOUND-FAILED") != std::string::npos, "unsolicited inbound from the allowed tuple's source port is dropped (" + trim(inbound) + ")");
            std::string const outbound_again = bpy(backend, *s1, probe_script({{"w9000", "198.51.100.2", "9000"}}), ctx);
            check(parse_probe(outbound_again, "w9000").find("WORLD-OK") != std::string::npos, "same sandbox: outbound to the allowed tuple still works (the drop is the ct rule, not a dead path)");
            evidence("unsolicited inbound dropped (ct state)", "control: " + trim(ctl_inbound), trim(inbound));
        }
        if (s1) destroy_box(backend, *s1);
        if (s2) destroy_box(backend, *s2);
    }
    check(ae_tables() == "0", "after the last sandbox is destroyed the shared table is gone");

    // ======================= PMTUD ==========================================================================
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s3 = make_box(backend, make_spec({"198.51.100.2:9000:http", "198.51.100.3:9000:http"}), ctx, "S3 (PMTUD)");
        if (s3) {
            (void)sh("ip link set aepvw0 mtu 1400");
            std::string const tag = s3->id();
            std::string const ok_run = bpy(backend, *s3, upload_script("198.51.100.2", 600000, tag + "-pm1"), ctx);
            check(ok_run.find("SENT 600000") != std::string::npos, "PMTUD: a 600 kB upload over a 1400-MTU path completes with the related-ICMP rule (" + trim(ok_run) + ")");
            // control: remove the related-ICMP rule from the output chain, then upload to the other allowed IP
            Sh chain = sh("nft -a list chain bridge ae_k out_" + tag);
            std::string handle;
            {
                std::istringstream in(chain.out);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.find("icmp") != std::string::npos && line.find("# handle ") != std::string::npos) handle = trim(line.substr(line.find("# handle ") + 9));
                }
            }
            check(!handle.empty(), "found the related-ICMP rule handle for the control");
            if (!handle.empty()) {
                (void)sh("nft delete rule bridge ae_k out_" + tag + " handle " + handle);
                std::string const stalled = guest_py(tag, upload_script("198.51.100.3", 600000, tag + "-pm2"), 80).out;
                long long const sent = first_int_after(stalled, "SENT");
                check(sent >= 0 && sent < 300000, "CONTROL: without the related-ICMP rule the same upload stalls (" + trim(stalled) + ")");
                evidence("PMTUD: frag-needed reaches the guest (related ICMP)", "rule deleted -> stalls after " + std::to_string(sent) + " bytes", trim(ok_run));
            }
            (void)sh("ip link set aepvw0 mtu 1500");
            destroy_box(backend, *s3);
        }
    }

    // ======================= net_bytes ======================================================================
    constexpr long long N = 200000;
    {
        KataBackend backend(kRuntime, kPythonImage);
        // no-quota control
        auto snq = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "Snq (no net_bytes: control)");
        if (snq) {
            std::string const up = bpy(backend, *snq, upload_script("198.51.100.2", 1000000, "nq"), ctx);
            std::string const down = bpy(backend, *snq, download_script("198.51.100.2", 1000000), ctx);
            check(up.find("SENT 1000000") != std::string::npos && first_int_after(down, "RECV") == 1000000,
                  "CONTROL (no net_bytes): the full 1000000 bytes move in both directions (" + trim(up) + " / " + trim(down) + ")");
            check(sh("nft -j list quota bridge ae_k q_" + snq->id()).rc != 0, "no quota object exists for a sandbox without net_bytes");
            destroy_box(backend, *snq);
        }
        // download: inbound counted
        auto sd = make_box(backend, make_spec({"198.51.100.2:9000:http"}, N), ctx, "Sd (net_bytes=200000, download)");
        if (sd) {
            std::string const down = bpy(backend, *sd, download_script("198.51.100.2", 1000000), ctx);
            long long const got = first_int_after(down, "RECV");
            long long const used = quota_used(sd->id());
            check(got > 0 && got <= N, "net_bytes counts INBOUND: a 1000000-byte download delivers " + std::to_string(got) + " <= N=" + std::to_string(N));
            check(used >= N, "quota used (" + std::to_string(used) + ") reached N after the download");
            evidence("net_bytes counts inbound bytes", "no-quota control moves 1000000", "delivered " + std::to_string(got) + " of N=" + std::to_string(N) + ", quota used " + std::to_string(used));
            destroy_box(backend, *sd);
        }
        // upload: outbound counted
        auto su = make_box(backend, make_spec({"198.51.100.2:9000:http"}, N), ctx, "Su (net_bytes=200000, upload)");
        if (su) {
            (void)bpy(backend, *su, upload_script("198.51.100.2", 1000000, "qup"), ctx);
            std::this_thread::sleep_for(std::chrono::seconds(10));
            long long const got = sink_total("qup");
            long long const used = quota_used(su->id());
            check(got > 0 && got <= N, "net_bytes counts OUTBOUND: the sink received " + std::to_string(got) + " <= N=" + std::to_string(N) + " of 1000000 sent");
            check(used >= N, "quota used (" + std::to_string(used) + ") reached N after the upload");
            evidence("net_bytes counts outbound bytes", "no-quota control delivers 1000000 to the sink", "sink got " + std::to_string(got) + " of N=" + std::to_string(N) + ", quota used " + std::to_string(used));
            destroy_box(backend, *su);
        }
        // shared across directions
        auto sb = make_box(backend, make_spec({"198.51.100.2:9000:http"}, N), ctx, "Sb (net_bytes=200000, shared budget)");
        if (sb) {
            (void)bpy(backend, *sb, upload_script("198.51.100.2", 120000, "qboth"), ctx);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            long long const up = sink_total("qboth");
            std::string const down = bpy(backend, *sb, download_script("198.51.100.2", 1000000), ctx);
            long long const got = first_int_after(down, "RECV");
            check(up == 120000, "shared budget: the 120000-byte upload fit under N (" + std::to_string(up) + ")");
            check(got >= 0 && up + got <= N, "shared budget: upload " + std::to_string(up) + " + download " + std::to_string(got) + " <= N=" + std::to_string(N));
            check(got < N - up + 1, "the download was cut by the budget the upload already consumed (each direction alone could use all of N)");
            evidence("net_bytes is one shared budget for both directions", "separate sandboxes: each direction alone can use ~N", "up " + std::to_string(up) + " + down " + std::to_string(got) + " <= " + std::to_string(N));
            destroy_box(backend, *sb);
        }
    }

    // ======================= leftovers + host firewall ======================================================
    check(ae_tables() == "0", "no ae_k table left");
    check(bridge_ports() == ports_before, "no bridge port left behind (before '" + ports_before + "' after '" + bridge_ports() + "')");
    check(netns_list() == netns_before, "no ae-kata/aepv netns left behind");
    check(ctr_containers() == containers_before, "no containerd container left behind");
    {
        std::string const fw_after = firewall_snapshot();
        bool const same = fw_after == fw_before;
        if (!same) {
            std::ofstream("/tmp/aepv-fw-before.txt") << fw_before;
            std::ofstream("/tmp/aepv-fw-after.txt") << fw_after;
            std::fprintf(stderr, "firewall diff:\n%s\n", sh("diff /tmp/aepv-fw-before.txt /tmp/aepv-fw-after.txt | head -40").out.c_str());
        }
        check(same, "host iptables + nft rules (outside ae_k) are byte-identical before and after the whole run (the backend never edits the host firewall)");
    }
    check(sh("ls /var/run/netns/ | grep -c '^aepv-c'").out.find('0') == 0, "control netns are gone");
    lab.down();

    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_backend_egress_linux: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_backend_egress_linux: %d FAILURE(S)\n", g_failures.load());
    return 1;
}
