// Host-free proof of the pure half of the Kata egress policy (ADR-240): the generators emit exactly the
// reviewed text and refuse hostile input (I3), the listing and CNI parsers behave, and HostEgress runs its
// steps in the fail-closed order and stops (and undoes) on a failure at any step. No VM, no root, no nft needed:
// every process goes through an injected runner. The host-side acceptance tests are
// test_kata_backend_egress_linux / _lifecycle_linux.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "backends/kata/egress_policy.hpp"

using namespace agentengine;
using namespace agentengine::kata::egress;

namespace {

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "ok: %s\n", what.c_str());
    }
}

SandboxPlan good_plan() {
    SandboxPlan p;
    p.tag = "ae-kata-1a2b3c";
    p.veth = "veth8187d324";
    p.bridge = "aekata0";
    p.guest_mac = "32:12:25:cd:83:9f";
    p.guest_ip = (10u << 24) | (88u << 16) | (77u << 8) | 5u;
    p.gateway_ip = (10u << 24) | (88u << 16) | (77u << 8) | 1u;
    p.allowed = {{(198u << 24) | (51u << 16) | (100u << 8) | 2u, 9000}, {(93u << 24) | (184u << 16) | (216u << 8) | 34u, 443}};
    p.net_bytes = 200000;
    return p;
}

bool contains(std::string const& hay, std::string const& needle) { return hay.find(needle) != std::string::npos; }

std::size_t line_of(std::string const& text, std::string const& needle) {
    std::size_t at = text.find(needle);
    return at == std::string::npos ? std::string::npos : static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<long>(at), '\n'));
}

// `nft list table bridge ae_k` as printed by nft 1.1.6 for the base table (captured on the prove host).
constexpr char kBaseListing[] = R"(table bridge ae_k {
	comment "agentengine-kata-egress-v1"
	counter frag_drops {
		packets 0 bytes 0
	}

	set ports {
		type ifname
		elements = { "veth1", "veth2" }
	}

	set bridges {
		type ifname
		elements = { "aekata0" }
	}

	map in_d {
		type ifname : verdict
		elements = { "veth1" : jump in_ae-kata-aa, "veth2" : jump in_ae-kata-bb }
	}

	map out_d {
		type ifname : verdict
		elements = { "veth1" : jump out_ae-kata-aa, "veth2" : jump out_ae-kata-bb }
	}

	chain c_pre {
		type filter hook prerouting priority -450; policy accept;
		iifname @ports ip frag-off & 0x3fff != 0x0 counter name "frag_drops" drop
	}

	chain c_input {
		type filter hook input priority -300; policy accept;
		iifname vmap @in_d
		meta ibrname @bridges drop
	}

	chain c_forward {
		type filter hook forward priority -300; policy accept;
		iifname @ports drop
		oifname @ports drop
		meta ibrname @bridges drop
	}

	chain c_output {
		type filter hook output priority -300; policy accept;
		oifname vmap @out_d
		meta obrname @bridges drop
	}

	chain in_ae-kata-aa {
		counter drop
	}
}
)";

constexpr char kCniResult[] = R"({"cniVersion":"1.0.0","interfaces":[{"name":"aekata0","mac":"da:f0:af:39:1b:70"},{"name":"veth8187d324","mac":"9a:11:22:33:44:55"},{"name":"eth0","mac":"32:12:25:CD:83:9F","sandbox":"/var/run/netns/ae-kata-1"}],"ips":[{"interface":2,"address":"10.88.77.5/24","gateway":"10.88.77.1"}],"routes":[{"dst":"0.0.0.0/0"}],"dns":{}})";

// A scripted runner: records every argv (and the content of any file passed to `nft -f` / `tc -batch`), fails the
// `fail_at`-th EFFECT command (bridge/tc/nft -f, counted from 1; 0 = never), answers listings with `listing`.
struct Script {
    std::vector<std::string> calls;
    std::vector<std::string> effect_files;  // file content seen at each effect call
    int effects = 0;
    int fail_at = 0;
    std::string listing_stdout;
    int listing_exit = 1;
    std::string listing_stderr = "Error: No such file or directory\n";
    std::string after_stdout;  // once `after_effects` effect steps ran, listings succeed with this text
    int after_effects = 4;
    [[nodiscard]] CommandRunner runner() {
        return [this](std::vector<std::string> const& argv) -> result<CommandOutcome> {
            std::string joined;
            for (auto const& a : argv) joined += (joined.empty() ? "" : " ") + a;
            calls.push_back(joined);
            CommandOutcome o;
            bool const is_list = argv.size() >= 2 && ((argv[0] == "nft" && argv[1] == "list") || (argv[0] == "tc" && argv[1] == "filter") ||
                                                      (argv[0] == "bridge" && (argv[1] == "-d" || argv[1] == "fdb") && argv.size() > 2 && argv[2] != "replace"));
            if (is_list && !after_stdout.empty() && effects >= after_effects) {
                o.exit_code = 0;
                o.stdout_text = after_stdout;
                return o;
            }
            if (is_list) {
                o.exit_code = listing_exit;
                o.stdout_text = listing_stdout;
                o.stderr_text = listing_stderr;
                return o;
            }
            ++effects;
            for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
                if (argv[i] == "-f" || argv[i] == "-batch") {
                    std::ifstream f(argv[i + 1]);
                    std::stringstream ss;
                    ss << f.rdbuf();
                    effect_files.push_back(ss.str());
                }
            }
            o.exit_code = (effects == fail_at) ? 1 : 0;
            if (o.exit_code != 0) o.stderr_text = "injected";
            return o;
        };
    }
};

}  // namespace

int main() {
    // ---------- validators / hostile input (I3) ----------
    for (char const* bad : {"a;b", "a b", "a\nb", "a\"b", "flush ruleset", "../x", "", "a{", "$(id)", "é"}) {
        check(!valid_tag(bad), std::string("valid_tag rejects hostile tag '") + (std::string(bad).find('\n') != std::string::npos ? "a\\nb" : bad) + "'");
    }
    check(!valid_tag(std::string(65, 'a')) && valid_tag(std::string(64, 'a')), "valid_tag length bound 64");
    for (char const* bad : {"", "-x", ".x", "..", "a b", "a;b", "0123456789abcdef", "a\"b", "a/b"}) check(!valid_ifname(bad), std::string("valid_ifname rejects '") + bad + "'");
    check(valid_ifname("veth8187d324") && valid_ifname("aekata0") && valid_ifname("0123456789abcde"), "valid_ifname accepts real CNI names");
    for (char const* bad : {"32:12:25:CD:83:9F", "32:12:25:cd:83", "33:12:25:cd:83:9f", "00:00:00:00:00:00", "32-12-25-cd-83-9f", "32:12:25:cd:83:9g", "32:12:25:cd:83:9f ", "01:00:5e:00:00:01"}) {
        check(!valid_mac(bad), std::string("valid_mac rejects '") + bad + "'");
    }
    check(valid_mac("32:12:25:cd:83:9f"), "valid_mac accepts a real guest MAC");

    {
        SandboxPlan p = good_plan();
        check(validate(p).has_value(), "validate accepts the good plan");
        auto reject = [&](char const* what, auto mutate) {
            SandboxPlan q = good_plan();
            mutate(q);
            check(!validate(q).has_value() && !render_install_script(q).has_value() && !render_tc_batch(q).has_value(), std::string("every generator refuses: ") + what);
        };
        reject("empty allowlist", [](SandboxPlan& q) { q.allowed.clear(); });
        reject("tag with ';'", [](SandboxPlan& q) { q.tag = "x; flush ruleset"; });
        reject("tag with newline", [](SandboxPlan& q) { q.tag = "x\nadd rule"; });
        reject("veth with quote", [](SandboxPlan& q) { q.veth = "v\" : jump x"; });
        reject("bridge with space", [](SandboxPlan& q) { q.bridge = "a b"; });
        reject("uppercase MAC", [](SandboxPlan& q) { q.guest_mac = "AA:BB:CC:DD:EE:FF"; });
        reject("multicast guest MAC", [](SandboxPlan& q) { q.guest_mac = "01:00:5e:00:00:01"; });
        reject("zero guest ip", [](SandboxPlan& q) { q.guest_ip = 0; });
        reject("loopback endpoint", [](SandboxPlan& q) { q.allowed[0].ipv4_host_order = (127u << 24) | 1u; });
        reject("multicast endpoint", [](SandboxPlan& q) { q.allowed[0].ipv4_host_order = (224u << 24) | 1u; });
        reject("port 0", [](SandboxPlan& q) { q.allowed[0].port = 0; });
        reject("duplicate endpoint", [](SandboxPlan& q) { q.allowed.push_back(q.allowed[0]); });
        reject("65 endpoints", [](SandboxPlan& q) {
            q.allowed.clear();
            for (std::uint32_t i = 0; i < 65; ++i) q.allowed.push_back({(198u << 24) | (51u << 16) | (100u << 8) | (i + 1), 9000});
        });
        check(!render_remove_script("a;b", "veth1").has_value() && !render_remove_script("ok", "v\"x").has_value(), "render_remove_script refuses hostile tag/ifname");
    }

    // ---------- golden text ----------
    {
        auto script = render_install_script(good_plan());
        check(script.has_value(), "install script renders");
        std::string const& s = *script;
        check(contains(s, "add chain bridge ae_k in_ae-kata-1a2b3c\n") && contains(s, "add chain bridge ae_k out_ae-kata-1a2b3c\n"), "sandbox chains");
        check(contains(s, "add quota bridge ae_k q_ae-kata-1a2b3c { over 200000 bytes }\n"), "named quota with N");
        check(contains(s, "add rule bridge ae_k in_ae-kata-1a2b3c ether saddr != 32:12:25:cd:83:9f counter name d_ae-kata-1a2b3c drop\n"), "source-MAC pin");
        check(contains(s, "ether type != { ip, arp } counter name d_ae-kata-1a2b3c drop"), "VLAN/QinQ/IPv6/other ethertypes dropped (nft sees through 802.1Q)");
        check(contains(s, "arp saddr ether 32:12:25:cd:83:9f arp saddr ip 10.88.77.5 arp daddr ip 10.88.77.1 accept"), "strict ARP to the gateway only");
        check(contains(s, "tcp flags & (syn | ack) == syn limit rate over 100/second burst 50 packets"), "SYN rate limit");
        check(contains(s, "ip saddr 10.88.77.5 ip daddr 198.51.100.2 tcp dport 9000 accept") && contains(s, "ip saddr 10.88.77.5 ip daddr 93.184.216.34 tcp dport 443 accept"), "one exact tuple rule per endpoint");
        check(contains(s, "ip saddr 198.51.100.2 ip daddr 10.88.77.5 tcp sport 9000 ct state established,related accept"), "return path requires ct state (no unsolicited inbound)");
        check(contains(s, "ip protocol icmp ct state related accept"), "related ICMP for PMTUD");
        check(line_of(s, "quota name q_ae-kata-1a2b3c counter name d_ae-kata-1a2b3c drop") < line_of(s, "tcp dport 9000 accept"), "quota drop precedes the accepts (nft stops at the first terminal verdict)");
        check(line_of(s, "arp operation request") < line_of(s, "quota name"), "ARP accepts precede the quota (ARP is never counted)");
        check(line_of(s, "add element bridge ae_k in_d") > line_of(s, "tcp dport 443 accept") && line_of(s, "add element bridge ae_k out_d") > line_of(s, "ct state established,related accept"),
              "map elements are added after the chains and rules");
        check(contains(s, "in_d { \"veth8187d324\" : jump in_ae-kata-1a2b3c }") && contains(s, "bridges { \"aekata0\" }") && contains(s, "ports { \"veth8187d324\" }"), "dispatch elements");
        SandboxPlan nq = good_plan();
        nq.net_bytes = 0;
        auto s2 = render_install_script(nq);
        check(s2.has_value() && !contains(*s2, "quota"), "no net_bytes -> no quota object and no quota rule");

        auto rm = render_remove_script("ae-kata-1a2b3c", "veth8187d324");
        check(rm.has_value() && contains(*rm, "destroy element bridge ae_k in_d { \"veth8187d324\" }") && contains(*rm, "destroy chain bridge ae_k in_ae-kata-1a2b3c") &&
                  contains(*rm, "destroy quota bridge ae_k q_ae-kata-1a2b3c") && !contains(*rm, "delete table") && !contains(*rm, "flush"),
              "remove script destroys only this sandbox's objects (idempotent `destroy`, never a table or flush)");

        auto tc = render_tc_batch(good_plan());
        check(tc.has_value(), "tc batch renders");
        std::string const& t = *tc;
        check(line_of(t, "qdisc add dev veth8187d324 clsact") == 0, "tc: qdisc first");
        check(line_of(t, "ingress pref 100 matchall action drop") < line_of(t, "action ok") && line_of(t, "egress pref 100 matchall action drop") < line_of(t, "action ok"),
              "tc: default-deny matchall (both directions) precedes EVERY allow (a partial batch is only more restrictive)");
        check(contains(t, "ingress pref 20 protocol ip flower src_mac 32:12:25:cd:83:9f src_ip 10.88.77.5 dst_ip 198.51.100.2 ip_proto tcp dst_port 9000 ip_flags nofrag action ok"), "tc: exact ingress tuple, no fragments");
        check(contains(t, "egress pref 21 protocol ip flower src_ip 93.184.216.34 dst_ip 10.88.77.5 ip_proto tcp src_port 443 ip_flags nofrag action ok"), "tc: exact egress return tuple");
        check(contains(t, "ip_proto icmp type 3"), "tc: only ICMP destination-unreachable to the guest");
        check(!contains(t, "pref 100 protocol"), "tc: nothing else at the default-deny pref");
    }

    // ---------- listing parsers ----------
    {
        check(base_table_matches(kBaseListing), "base_table_matches accepts the real nft listing of the base table");
        std::string tampered = kBaseListing;
        tampered.replace(tampered.find("iifname @ports drop"), 19, "iifname @ports drop\n\t\taccept");
        check(!base_table_matches(tampered), "base_table_matches rejects an extra rule in a base chain");
        std::string missing = kBaseListing;
        missing.replace(missing.find("meta ibrname @bridges drop"), 26, "");
        check(!base_table_matches(missing), "base_table_matches rejects a missing default-drop rule");
        std::string nocomment = kBaseListing;
        nocomment.replace(nocomment.find("agentengine-kata-egress-v1"), 26, "someone-elses-table");
        check(!base_table_matches(nocomment), "base_table_matches rejects a foreign table of the same name");
        auto elems = parse_map_elements(extract_block(kBaseListing, "map in_d"));
        check(elems.size() == 2 && elems[0].veth == "veth1" && elems[0].chain == "in_ae-kata-aa" && elems[1].veth == "veth2" && elems[1].chain == "in_ae-kata-bb", "parse_map_elements reads vmap elements");
        check(extract_block(kBaseListing, "chain in_ae-kata-aa") == "\tchain in_ae-kata-aa {\n\t\tcounter drop\n\t}\n", "extract_block returns exactly the chain block");
        check(extract_block(kBaseListing, "chain in_nope").empty(), "extract_block returns empty for a missing chain");
    }

    // ---------- CNI result ----------
    {
        auto att = parse_cni_result(kCniResult);
        check(att.has_value() && att->guest_mac == "32:12:25:cd:83:9f" && att->guest_ip == ((10u << 24) | (88u << 16) | (77u << 8) | 5u) &&
                  att->gateway_ip == ((10u << 24) | (88u << 16) | (77u << 8) | 1u) && att->host_interface_candidates.size() == 2,
              "parse_cni_result: guest MAC lowercased, address, gateway, two host candidates (bridge + veth)");
        for (auto const& [why, text] : std::vector<std::pair<std::string, std::string>>{
                 {"not JSON", "oops"},
                 {"no interfaces", R"({"ips":[]})"},
                 {"two ips", R"({"interfaces":[{"name":"eth0","mac":"32:12:25:cd:83:9f","sandbox":"x"}],"ips":[{"address":"10.0.0.2/24","gateway":"10.0.0.1"},{"address":"10.0.0.3/24","gateway":"10.0.0.1"}]})"},
                 {"IPv6 only", R"({"interfaces":[{"name":"eth0","mac":"32:12:25:cd:83:9f","sandbox":"x"}],"ips":[{"address":"fd00::2/64","gateway":"fd00::1"}]})"},
                 {"no sandbox interface", R"({"interfaces":[{"name":"veth1","mac":"32:12:25:cd:83:9f"}],"ips":[{"address":"10.0.0.2/24","gateway":"10.0.0.1"}]})"},
                 {"multicast guest MAC", R"({"interfaces":[{"name":"eth0","mac":"01:00:5e:00:00:01","sandbox":"x"}],"ips":[{"address":"10.0.0.2/24","gateway":"10.0.0.1"}]})"},
                 {"missing gateway", R"({"interfaces":[{"name":"eth0","mac":"32:12:25:cd:83:9f","sandbox":"x"}],"ips":[{"address":"10.0.0.2/24"}]})"},
                 {"octal-looking address", R"({"interfaces":[{"name":"eth0","mac":"32:12:25:cd:83:9f","sandbox":"x"}],"ips":[{"address":"010.0.0.2/24","gateway":"10.0.0.1"}]})"}}) {
            auto r = parse_cni_result(text);
            check(!r.has_value() && r.error().code == "kata_backend.egress_topology_unsupported", "parse_cni_result fails closed: " + why);
        }
    }

    // ---------- HostEgress order and failure behaviour (scripted runner, no host) ----------
    {
        std::string const dir = "/tmp/aepv-unit-egress-" + std::to_string(::getpid());
        SandboxPlan p = good_plan();
        p.veth = "lo";  // an interface that exists everywhere, so HostEgress::port_exists() is true without a host setup
        {
            Script s;
            s.after_stdout = kBaseListing;  // after the base table is loaded it 'exists' but holds none of OUR chains
            HostEgress he(s.runner(), dir);
            auto r = he.install(p);  // read-back finds no sandbox chains -> install fails, which is what we observe
            check(!r.has_value(), "scripted install whose read-back lacks the sandbox chains fails (cannot prove its own policy)");
            auto idx = [&](std::string const& prefix) {
                for (std::size_t i = 0; i < s.calls.size(); ++i) {
                    if (s.calls[i].rfind(prefix, 0) == 0) return static_cast<long>(i);
                }
                return -1L;
            };
            long const flags = idx("bridge link set dev lo learning off locked on isolated on");
            long const fdb = idx("bridge fdb replace 32:12:25:cd:83:9f dev lo master static");
            long const tcb = idx("tc -batch");
            long nft_sb = -1;
            for (std::size_t i = 0; i < s.calls.size(); ++i) {
                if (s.calls[i].find("/sb-ae-kata-1a2b3c.nft") != std::string::npos) nft_sb = static_cast<long>(i);
            }
            check(flags >= 0 && fdb > flags && tcb > fdb && nft_sb > tcb, "install order: port flags -> static FDB -> tc batch -> nft sandbox transaction");
            check(s.effect_files.size() >= 3 && contains(s.effect_files[1], "chain c_input") && contains(s.effect_files[2], "add chain bridge ae_k in_ae-kata-1a2b3c"),
                  "the base table is loaded before the sandbox transaction");
            check(r.error().code == "kata_backend.egress_policy_install_failed", "the failure carries kata_backend.egress_policy_install_failed");
            bool undone = false;
            for (auto const& f : s.effect_files) undone = undone || (contains(f, "destroy chain bridge ae_k in_ae-kata-1a2b3c") && contains(f, "destroy element"));
            check(undone, "a failed read-back undoes this sandbox's committed nft state");
        }
        // inject a failure at each effect step: bridge link, bridge fdb, tc, nft base, nft sandbox
        char const* names[] = {"bridge link set", "bridge fdb replace", "tc -batch", "nft base", "nft sandbox"};
        for (int k = 1; k <= 5; ++k) {
            Script s;
            s.fail_at = k;
            HostEgress he(s.runner(), dir);
            auto r = he.install(p);
            check(!r.has_value() && r.error().code == "kata_backend.egress_policy_install_failed", std::string("injected failure at ") + names[k - 1] + " -> install fails closed");
            check(s.effects == k, std::string("no further effect step ran after the failure at ") + names[k - 1] + " (effects=" + std::to_string(s.effects) + ")");
        }
        // remove() refuses while the port exists, runs when it does not
        {
            Script s;
            HostEgress he(s.runner(), dir);
            auto r = he.remove("ae-kata-1a2b3c", "lo");
            check(!r.has_value() && r.error().code == "kata_backend.egress_policy_kept" && s.calls.empty(), "remove() of a still-present port is refused and runs NOTHING (the policy outlives the port)");
            auto r2 = he.remove("ae-kata-1a2b3c", "aepvnoport0");
            check(r2.has_value(), "remove() of a port that is gone succeeds (no table: nothing of ours remains)");
        }
        // an `nft list table` failure that is not 'No such file' must NOT be treated as 'table absent'
        {
            Script s;
            s.listing_exit = 1;
            s.listing_stderr = "Error: Operation not permitted\n";
            HostEgress he(s.runner(), dir);
            auto r = he.install(p);
            bool loaded_base = false;
            for (auto const& f : s.effect_files) loaded_base = loaded_base || contains(f, "chain c_input");
            check(!r.has_value() && !loaded_base, "an unreadable table is not mistaken for an absent one (the base table is never re-loaded over a live one)");
        }
        std::error_code rm_ec;
        std::filesystem::remove_all(dir, rm_ec);
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_egress_policy_render: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_egress_policy_render: %d FAILURE(S)\n", g_failures);
    return 1;
}
