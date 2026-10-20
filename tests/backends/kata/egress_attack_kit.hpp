#pragma once
// Hostile-guest attack batch used by the Kata egress acceptance tests (ADR-240). One guest exec sends every
// frame in kAttackBody with CAP_NET_RAW (all the backend grants a guest), while host-side sniffers on the
// Kata bridge device (what reached the host) and on the world veth (what was forwarded out) count markers.
// The same batch runs against a hand-wired control sandbox (no policy: the markers must show up, otherwise the
// probe cannot detect a hole) and against policy sandboxes (every marker must be absent except the two
// legitimate ones, which prove the sniffers and the sender still work under policy).

#include <map>
#include <string>
#include <vector>

#include "egress_test_support.hpp"

namespace aepv {

struct AttackTarget {
    std::string attacker_mac;
    std::string attacker_ip;
    std::string victim_mac;  // another sandbox's guest (spoofed source MAC / IP)
    std::string victim_ip;
    std::string gateway_mac;
    std::string gateway_ip = "10.88.77.1";
    std::string world_ip = "198.51.100.2";
};

// Markers that must NOT be delivered under policy; LEGIT and ARPOK must be.
inline std::vector<std::string> const& blocked_markers() {
    static std::vector<std::string> const m{"AEPV-FRAG1", "AEPV-FRAG2", "AEPV-VLAN", "AEPV-UDP", "AEPV-SPIP", "AEPV-SPMAC", "AEPV-ICMP",
                                             "AEPV-IPIP", "AEPV-GRE", "AEPV-IPV6", "AEPV-RAWETH", "AEPV-ARPSP", "AEPV-FDBS"};
    return m;
}
inline std::vector<std::string> const& legit_markers() {
    static std::vector<std::string> const m{"AEPV-LEGIT", "AEPV-ARPOK"};
    return m;
}

inline constexpr char kAttackBody[] = R"PY(
def M(s): return ('AEPV-' + s).encode()
def udp(src, dst, sp, dp, data):
    return struct.pack('!HHHH', sp, dp, 8 + len(data), 0) + data
W = WORLD
# 1 first fragment of a segment to the allowed tuple (32 bytes of payload: multiple of 8 for MF=1)
t = tcp(MYIP, W, 40001, 9000, b'')
snd(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 6, t + b'AEPV-FRAG1' + b'..', ident=77, mf=1, off=0)), 3)
# 2 non-first fragment whose bytes 2..4 look like dport 9000 (nft's tcp match does not exclude non-first fragments)
snd(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 6, struct.pack('!HH', 40002, 9000) + b'\0' * 4 + b'AEPV-FRAG2', ident=78, mf=0, off=1)), 3)
# 3 802.1Q tagged frame carrying a segment to the allowed tuple
inner = ip4(MYIP, W, 6, tcp(MYIP, W, 40003, 9000, M('VLAN')))
snd(eth(GWMAC, MYMAC, 0x8100, struct.pack('!HH', 5, 0x0800) + inner), 3)
# 4 UDP to the allowed ip:port
snd(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 17, udp(MYIP, W, 40004, 9000, M('UDP')))), 3)
# 5 spoofed source IP (another sandbox's address)
snd(eth(GWMAC, MYMAC, 0x0800, ip4(VIP, W, 6, tcp(VIP, W, 40005, 9000, M('SPIP')))), 3)
# 6 spoofed source MAC with the right IP
snd(eth(GWMAC, '02:de:ad:be:ef:01', 0x0800, ip4(MYIP, W, 6, tcp(MYIP, W, 40006, 9000, M('SPMAC')))), 3)
# 7 ICMP echo to the allowed address
ic = struct.pack('!BBHHH', 8, 0, 0, 1, 1) + M('ICMP')
ic = ic[:2] + struct.pack('!H', cs(ic)) + ic[4:]
snd(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 1, ic)), 3)
# 8 IP-in-IP and 9 GRE to the allowed address
snd(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 4, ip4(MYIP, W, 6, tcp(MYIP, W, 40008, 9000, M('IPIP'))))), 3)
snd(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 47, b'\0\0\x08\x00' + ip4(MYIP, W, 6, tcp(MYIP, W, 40009, 9000, M('GRE'))))), 3)
# 10 an IPv6 ethertype frame and 11 a raw non-IP ethertype frame to the bridge
snd(eth(GWMAC, MYMAC, 0x86dd, b'\x60' + b'\0' * 39 + M('IPV6')), 3)
snd(eth(GWMAC, MYMAC, 0x88b5, M('RAWETH')), 3)
# 12 ARP request claiming another sandbox's IP (poisons the host neighbour table when it gets through)
snd(eth('ff:ff:ff:ff:ff:ff', MYMAC, 0x0806, arp(1, MYMAC, VIP, '00:00:00:00:00:00', GWIP) + M('ARPSP')), 3)
# 13 frames sourced from the victim's MAC (bridge FDB poisoning: learning runs before netfilter)
snd(eth(GWMAC, VMAC, 0x88b5, M('FDBS')), 3)
# legitimate traffic: ARP for the gateway with the guest's own identity, and a SYN to the allowed tuple
snd(eth('ff:ff:ff:ff:ff:ff', MYMAC, 0x0806, arp(1, MYMAC, MYIP, '00:00:00:00:00:00', GWIP) + M('ARPOK')), 3)
snd(eth(GWMAC, MYMAC, 0x0800, ip4(MYIP, W, 6, tcp(MYIP, W, 40010, 9000, M('LEGIT')))), 3)
time.sleep(1)
print('ATTACK-DONE')
)PY";

inline std::string attack_script(AttackTarget const& t) {
    std::ostringstream o;
    o << "MYMAC='" << t.attacker_mac << "'\nMYIP='" << t.attacker_ip << "'\nVMAC='" << t.victim_mac << "'\nVIP='" << t.victim_ip
      << "'\nGWMAC='" << t.gateway_mac << "'\nGWIP='" << t.gateway_ip << "'\nWORLD='" << t.world_ip << "'\n"
      << kGuestLib << kAttackBody;
    return o.str();
}

// Runs the batch in `guest_id` and returns marker -> number of frames seen (bridge device + world veth summed).
inline std::map<std::string, int> run_attack_batch(std::string const& guest_id, AttackTarget const& t) {
    std::vector<std::string> markers = blocked_markers();
    for (auto const& m : legit_markers()) markers.push_back(m);
    auto s_bridge = Lab::sniff_start("aekata0", 15, markers);
    auto s_world = Lab::sniff_start("aepvw0", 15, markers);
    Sh r = guest_py(guest_id, attack_script(t), 80);
    if (!r.has("ATTACK-DONE")) std::fprintf(stderr, "attack batch did not finish: %s\n", r.out.c_str());
    auto a = Lab::sniff_collect(s_bridge);
    auto b = Lab::sniff_collect(s_world);
    std::map<std::string, int> sum;
    std::string detail;
    for (auto const& m : markers) {
        int const on_bridge = a.count(m) ? a[m] : 0;
        int const on_world = b.count(m) ? b[m] : 0;
        sum[m] = on_bridge + on_world;
        if (sum[m] != 0) detail += m.substr(5) + "(bridge=" + std::to_string(on_bridge) + ",world=" + std::to_string(on_world) + ") ";
    }
    std::fprintf(stderr, "attack batch per-interface: %s\n", detail.c_str());
    return sum;
}

inline std::string counts_line(std::map<std::string, int> const& c) {
    std::string out;
    for (auto const& [k, v] : c) out += k.substr(5) + "=" + std::to_string(v) + " ";
    return out;
}

}  // namespace aepv
