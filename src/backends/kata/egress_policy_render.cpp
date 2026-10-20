// Pure half of the Kata egress policy: validators, nft/tc script generators, listing parsers and the CNI result
// parser. No process is spawned and no file is touched here. Implements ADR-239 (see egress_policy.hpp's top
// comment for the design record). I3: every emitted token is a typed value or passed a charset check below.
#include "backends/kata/egress_policy.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "agentengine/core/json_value.hpp"

namespace agentengine::kata::egress {

namespace {

[[nodiscard]] error policy_error(std::string message, char const* code) {
    return error{failure_class::policy, std::move(message), code};
}

[[nodiscard]] bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

[[nodiscard]] bool parse_dotted(std::string_view s, std::uint32_t& out) {
    std::uint32_t value = 0;
    int octets = 0;
    std::size_t i = 0;
    while (i <= s.size()) {
        std::size_t j = i;
        unsigned part = 0;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            part = part * 10 + static_cast<unsigned>(s[j] - '0');
            if (part > 255 || j - i > 2) return false;
            ++j;
        }
        if (j == i) return false;
        if (s.size() - i > 1 && s[i] == '0' && j - i > 1) return false;  // no leading zeros (octal ambiguity)
        value = (value << 8) | part;
        ++octets;
        if (j == s.size()) break;
        if (s[j] != '.') return false;
        i = j + 1;
    }
    if (octets != 4) return false;
    out = value;
    return true;
}

[[nodiscard]] bool endpoint_sane(std::uint32_t ip) {
    std::uint32_t const top = ip >> 24;
    return ip != 0 && top != 0 && top != 127 && top < 224;
}

}  // namespace

bool valid_tag(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-'; });
}

bool valid_ifname(std::string_view s) {
    if (s.empty() || s.size() > 15) return false;  // IFNAMSIZ - 1
    if (s == "." || s == "..") return false;
    if (s.front() == '-' || s.front() == '.') return false;
    return std::all_of(s.begin(), s.end(), is_name_char);
}

bool valid_mac(std::string_view s) {
    if (s.size() != 17) return false;
    auto const hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    bool any_nonzero = false;
    for (std::size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (s[i] != ':') return false;
            continue;
        }
        int const v = hex(s[i]);
        if (v < 0) return false;
        if (v != 0) any_nonzero = true;
    }
    int const first_octet = hex(s[0]) * 16 + hex(s[1]);
    return any_nonzero && (first_octet & 1) == 0;  // unicast only
}

std::string ipv4_dotted(std::uint32_t v) {
    std::ostringstream oss;
    oss << ((v >> 24) & 0xFFu) << '.' << ((v >> 16) & 0xFFu) << '.' << ((v >> 8) & 0xFFu) << '.' << (v & 0xFFu);
    return oss.str();
}

result<std::monostate> validate(SandboxPlan const& p) {
    if (!valid_tag(p.tag)) return std::unexpected(policy_error("egress: invalid sandbox tag", "kata_backend.egress_policy_install_failed"));
    if (!valid_ifname(p.veth) || !valid_ifname(p.bridge)) {
        return std::unexpected(policy_error("egress: invalid interface name", "kata_backend.egress_policy_install_failed"));
    }
    if (!valid_mac(p.guest_mac)) return std::unexpected(policy_error("egress: invalid guest MAC", "kata_backend.egress_policy_install_failed"));
    if (!endpoint_sane(p.guest_ip) || !endpoint_sane(p.gateway_ip)) {
        return std::unexpected(policy_error("egress: invalid guest or gateway address", "kata_backend.egress_policy_install_failed"));
    }
    if (p.allowed.empty() || p.allowed.size() > kMaxEndpointsTotal) {
        return std::unexpected(policy_error("egress: allowed endpoint count out of range (1.." + std::to_string(kMaxEndpointsTotal) + ")",
                                            "kata_backend.egress_policy_install_failed"));
    }
    for (std::size_t i = 0; i < p.allowed.size(); ++i) {
        Endpoint const& e = p.allowed[i];
        if (!endpoint_sane(e.ipv4_host_order) || e.port == 0) {
            return std::unexpected(policy_error("egress: invalid allowed endpoint", "kata_backend.egress_policy_install_failed"));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (p.allowed[j] == e) return std::unexpected(policy_error("egress: duplicate allowed endpoint", "kata_backend.egress_policy_install_failed"));
        }
    }
    return std::monostate{};
}

std::string render_base_table() {
    std::ostringstream o;
    o << "table bridge " << kTableName << " {\n"
      << "\tcomment \"" << kTableComment << "\"\n"
      << "\tset ports { type ifname; }\n"
      << "\tset bridges { type ifname; }\n"
      << "\tmap in_d { type ifname : verdict; }\n"
      << "\tmap out_d { type ifname : verdict; }\n"
      << "\tcounter frag_drops { }\n"
      << "\tchain c_pre {\n"
      << "\t\ttype filter hook prerouting priority -450; policy accept;\n"
      << "\t\tiifname @ports ip frag-off & 0x3fff != 0 counter name frag_drops drop\n"
      << "\t}\n"
      << "\tchain c_input {\n"
      << "\t\ttype filter hook input priority -300; policy accept;\n"
      << "\t\tiifname vmap @in_d\n"
      << "\t\tmeta ibrname @bridges drop\n"
      << "\t}\n"
      << "\tchain c_forward {\n"
      << "\t\ttype filter hook forward priority -300; policy accept;\n"
      << "\t\tiifname @ports drop\n"
      << "\t\toifname @ports drop\n"
      << "\t\tmeta ibrname @bridges drop\n"
      << "\t}\n"
      << "\tchain c_output {\n"
      << "\t\ttype filter hook output priority -300; policy accept;\n"
      << "\t\toifname vmap @out_d\n"
      << "\t\tmeta obrname @bridges drop\n"
      << "\t}\n"
      << "}\n";
    return o.str();
}

result<std::string> render_install_script(SandboxPlan const& p) {
    if (auto v = validate(p); !v) return std::unexpected(v.error());
    std::string const t(kTableName);
    std::string const in = "in_" + p.tag;
    std::string const out = "out_" + p.tag;
    std::string const ctr = "d_" + p.tag;
    std::string const quota = "q_" + p.tag;
    std::string const mac = p.guest_mac;
    std::string const ip = ipv4_dotted(p.guest_ip);
    std::string const gw = ipv4_dotted(p.gateway_ip);
    std::ostringstream o;
    auto const rule = [&](std::string const& chain) -> std::ostringstream& {
        o << "add rule bridge " << t << ' ' << chain << ' ';
        return o;
    };
    o << "add chain bridge " << t << ' ' << in << '\n';
    o << "add chain bridge " << t << ' ' << out << '\n';
    o << "add counter bridge " << t << ' ' << ctr << '\n';
    if (p.net_bytes > 0) o << "add quota bridge " << t << ' ' << quota << " { over " << p.net_bytes << " bytes }\n";

    // frames from the guest toward the bridge/host
    rule(in) << "ether saddr != " << mac << " counter name " << ctr << " drop\n";
    // Only IPv4 and ARP may follow. Found in the prove stage: nft's bridge-family `ip` matches see THROUGH an
    // 802.1Q tag, so without this rule a VLAN-tagged frame carrying an allowed tuple is accepted by the tuple
    // rule below (VLAN, QinQ, IPv6 and every other ethertype are dropped here).
    rule(in) << "ether type != { ip, arp } counter name " << ctr << " drop\n";
    rule(in) << "ether type arp arp operation request arp saddr ether " << mac << " arp saddr ip " << ip << " arp daddr ip " << gw << " accept\n";
    rule(in) << "ether type arp arp operation reply arp saddr ether " << mac << " arp saddr ip " << ip << " arp daddr ip " << gw << " accept\n";
    rule(in) << "ether type ip ip protocol tcp tcp flags & (syn | ack) == syn limit rate over " << kSynRatePerSecond << "/second burst "
             << kSynBurstPackets << " packets counter name " << ctr << " drop\n";
    if (p.net_bytes > 0) rule(in) << "quota name " << quota << " counter name " << ctr << " drop\n";
    for (Endpoint const& e : p.allowed) {
        rule(in) << "ip saddr " << ip << " ip daddr " << ipv4_dotted(e.ipv4_host_order) << " tcp dport " << e.port << " accept\n";
    }
    rule(in) << "counter name " << ctr << " drop\n";

    // frames from the host toward the guest
    rule(out) << "ether type arp arp operation reply arp daddr ip " << ip << " accept\n";
    rule(out) << "ether type arp arp operation request arp saddr ip " << gw << " arp daddr ip " << ip << " accept\n";
    if (p.net_bytes > 0) rule(out) << "quota name " << quota << " counter name " << ctr << " drop\n";
    rule(out) << "ip protocol icmp ct state related accept\n";
    for (Endpoint const& e : p.allowed) {
        rule(out) << "ip saddr " << ipv4_dotted(e.ipv4_host_order) << " ip daddr " << ip << " tcp sport " << e.port
                  << " ct state established,related accept\n";
    }
    rule(out) << "counter name " << ctr << " drop\n";

    // elements last: the sandbox is dispatched only once its chains exist (one transaction anyway)
    o << "add element bridge " << t << " bridges { \"" << p.bridge << "\" }\n";
    o << "add element bridge " << t << " ports { \"" << p.veth << "\" }\n";
    o << "add element bridge " << t << " in_d { \"" << p.veth << "\" : jump " << in << " }\n";
    o << "add element bridge " << t << " out_d { \"" << p.veth << "\" : jump " << out << " }\n";
    return o.str();
}

result<std::string> render_remove_script(std::string_view tag, std::string_view veth) {
    if (!valid_tag(tag) || !valid_ifname(veth)) {
        return std::unexpected(policy_error("egress: invalid tag or interface name for removal", "kata_backend.egress_policy_install_failed"));
    }
    std::string const t(kTableName);
    std::string const in = "in_" + std::string(tag);
    std::string const out = "out_" + std::string(tag);
    std::ostringstream o;
    // `destroy` ignores a missing object, so a repeated or partial removal is a no-op (idempotent).
    o << "destroy element bridge " << t << " in_d { \"" << veth << "\" }\n";
    o << "destroy element bridge " << t << " out_d { \"" << veth << "\" }\n";
    o << "destroy element bridge " << t << " ports { \"" << veth << "\" }\n";
    o << "destroy chain bridge " << t << ' ' << in << '\n';
    o << "destroy chain bridge " << t << ' ' << out << '\n';
    o << "destroy counter bridge " << t << " d_" << tag << '\n';
    o << "destroy quota bridge " << t << " q_" << tag << '\n';
    return o.str();
}

result<std::string> render_tc_batch(SandboxPlan const& p) {
    if (auto v = validate(p); !v) return std::unexpected(v.error());
    std::string const dev = p.veth;
    std::string const m = p.guest_mac;
    std::string const ip = ipv4_dotted(p.guest_ip);
    std::string const gw = ipv4_dotted(p.gateway_ip);
    std::ostringstream o;
    o << "qdisc add dev " << dev << " clsact\n";
    // default-deny FIRST: every later line only adds an allow, so a partial batch is more restrictive
    o << "filter add dev " << dev << " ingress pref 100 matchall action drop\n";
    o << "filter add dev " << dev << " egress pref 100 matchall action drop\n";
    o << "filter add dev " << dev << " ingress pref 11 protocol arp flower src_mac " << m << " arp_op reply arp_sha " << m << " arp_sip " << ip
      << " arp_tip " << gw << " action ok\n";
    o << "filter add dev " << dev << " ingress pref 12 protocol arp flower src_mac " << m << " arp_op request arp_sha " << m << " arp_sip " << ip
      << " arp_tip " << gw << " action ok\n";
    o << "filter add dev " << dev << " egress pref 10 protocol arp flower arp_op reply arp_tip " << ip << " action ok\n";
    o << "filter add dev " << dev << " egress pref 11 protocol arp flower arp_op request arp_sip " << gw << " arp_tip " << ip << " action ok\n";
    o << "filter add dev " << dev << " egress pref 12 protocol ip flower dst_ip " << ip << " ip_proto icmp type 3 ip_flags nofrag action ok\n";
    unsigned pref = 20;  // endpoint filters stay below the pref-100 default drop (max 64 endpoints: 20..83)
    for (Endpoint const& e : p.allowed) {
        o << "filter add dev " << dev << " ingress pref " << pref << " protocol ip flower src_mac " << m << " src_ip " << ip << " dst_ip "
          << ipv4_dotted(e.ipv4_host_order) << " ip_proto tcp dst_port " << e.port << " ip_flags nofrag action ok\n";
        o << "filter add dev " << dev << " egress pref " << pref << " protocol ip flower src_ip " << ipv4_dotted(e.ipv4_host_order) << " dst_ip " << ip
          << " ip_proto tcp src_port " << e.port << " ip_flags nofrag action ok\n";
        ++pref;
    }
    return o.str();
}

std::string extract_block(std::string_view listing, std::string_view header) {
    std::string const needle = "\t" + std::string(header) + " {\n";
    std::size_t at = listing.find(needle);
    if (at == std::string_view::npos) return {};
    std::size_t const end = listing.find("\n\t}\n", at);
    if (end == std::string_view::npos) return {};
    return std::string(listing.substr(at, end + 4 - at));
}

std::size_t count_lines(std::string_view block) {
    return static_cast<std::size_t>(std::count(block.begin(), block.end(), '\n'));
}

std::vector<MapElement> parse_map_elements(std::string_view s) {
    std::vector<MapElement> out;
    std::size_t pos = 0;
    constexpr std::string_view kSep = "\" : jump ";
    for (;;) {
        std::size_t const sep = s.find(kSep, pos);
        if (sep == std::string_view::npos) break;
        std::size_t const key_open = s.rfind('"', sep == 0 ? 0 : sep - 1);
        std::size_t const chain_begin = sep + kSep.size();
        std::size_t chain_end = chain_begin;
        while (chain_end < s.size() && is_name_char(s[chain_end])) ++chain_end;
        if (key_open != std::string_view::npos && key_open < sep) {
            out.push_back(MapElement{std::string(s.substr(key_open + 1, sep - key_open - 1)),
                                     std::string(s.substr(chain_begin, chain_end - chain_begin))});
        }
        pos = chain_end;
    }
    return out;
}

bool base_table_matches(std::string_view l) {
    if (l.find("comment \"" + std::string(kTableComment) + "\"") == std::string_view::npos) return false;
    struct Expect {
        char const* header;
        std::size_t lines;  // header line + body lines + closing brace line
        std::vector<char const*> keys;
    };
    std::vector<Expect> const expectations{
        {"chain c_pre", 4, {"hook prerouting priority -450; policy accept;", "iifname @ports ip frag-off & 0x3fff != 0x0 counter name \"frag_drops\" drop"}},
        {"chain c_input", 5, {"hook input priority -300; policy accept;", "iifname vmap @in_d\n", "meta ibrname @bridges drop"}},
        {"chain c_forward", 6, {"hook forward priority -300; policy accept;", "iifname @ports drop", "oifname @ports drop", "meta ibrname @bridges drop"}},
        {"chain c_output", 5, {"hook output priority -300; policy accept;", "oifname vmap @out_d\n", "meta obrname @bridges drop"}},
    };
    for (Expect const& e : expectations) {
        std::string const block = extract_block(l, e.header);
        if (block.empty() || count_lines(block) != e.lines) return false;
        for (char const* k : e.keys) {
            if (block.find(k) == std::string::npos) return false;
        }
    }
    for (char const* h : {"set ports", "set bridges", "map in_d", "map out_d", "counter frag_drops"}) {
        if (extract_block(l, h).empty()) return false;
    }
    return true;
}

result<CniAttachment> parse_cni_result(std::string_view text) {
    auto const bad = [](char const* why) {
        return std::unexpected(error{failure_class::policy, std::string("egress: unusable CNI result: ") + why,
                                      "kata_backend.egress_topology_unsupported"});
    };
    auto parsed = agentengine::json::parse(text);
    if (!parsed || !parsed->is_object()) return bad("not a JSON object");
    auto const* interfaces = parsed->find("interfaces");
    auto const* ips = parsed->find("ips");
    if (interfaces == nullptr || !interfaces->is_array() || ips == nullptr || !ips->is_array()) return bad("no interfaces/ips");

    CniAttachment att;
    int guest_ifaces = 0;
    for (auto const& itf : interfaces->as_array()) {
        if (!itf.is_object()) return bad("interface entry is not an object");
        auto const* name = itf.find("name");
        auto const* sandbox = itf.find("sandbox");
        auto const* mac = itf.find("mac");
        if (name == nullptr || !name->is_string()) return bad("interface without a name");
        if (sandbox != nullptr && sandbox->is_string() && !sandbox->as_string().empty()) {
            ++guest_ifaces;
            if (mac == nullptr || !mac->is_string()) return bad("guest interface without a MAC");
            std::string m = mac->as_string();
            std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            att.guest_mac = std::move(m);
        } else {
            att.host_interface_candidates.push_back(name->as_string());
        }
    }
    if (guest_ifaces != 1) return bad("expected exactly one sandbox interface");
    if (!valid_mac(att.guest_mac)) return bad("guest MAC malformed");
    if (ips->as_array().size() != 1) return bad("expected exactly one ip entry (IPv4 only)");
    auto const& ip_entry = ips->as_array().front();
    auto const* addr = ip_entry.find("address");
    auto const* gw = ip_entry.find("gateway");
    if (addr == nullptr || !addr->is_string() || gw == nullptr || !gw->is_string()) return bad("ip entry without address/gateway");
    std::string_view a = addr->as_string();
    std::size_t const slash = a.find('/');
    if (slash == std::string_view::npos || !parse_dotted(a.substr(0, slash), att.guest_ip)) return bad("guest address is not an IPv4 CIDR");
    if (!parse_dotted(gw->as_string(), att.gateway_ip)) return bad("gateway is not an IPv4 address");
    if (!endpoint_sane(att.guest_ip) || !endpoint_sane(att.gateway_ip)) return bad("guest/gateway address out of range");
    return att;
}

}  // namespace agentengine::kata::egress
