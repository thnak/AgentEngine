// Lifecycle and fail-closed acceptance tests for the KataBackend egress policy (ADR-240, GitHub #194): table
// loss and tampering (the B1 blocker), exec-time verification, teardown, sweep ownership, parallel create/
// destroy, create failure paths (injected), idempotent install, A-record cap and the host-local entry check.
// Every check has a positive control; see egress_test_support.hpp for the lab and preconditions.
// REQUIRES: root, containerd + Kata, CNI ae-kata-net, cnitool, nft/tc/bridge/ip, python3, python:3.12-alpine
// pulled, FORWARD accepts for aekata0 <-> aepvw0 (comment ae-prove-temp).
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <set>
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
    ctx.trace_id = "kata-backend-egress-lifecycle-test";
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
    std::string veth;
    [[nodiscard]] std::string const& id() const { return handle.opaque_id; }
};

std::optional<Box> make_box(KataBackend& be, SandboxSpec const& spec, EffectContext& ctx, std::string const& label) {
    auto h = agentengine::rt::block_on(be.create(spec, ctx));
    check(h.has_value(), "create(): " + label + (h ? "" : " -- " + h.error().message));
    if (!h) return std::nullopt;
    Box b{*h, {}, {}};
    b.facts = guest_facts(b.id());
    b.veth = fdb_dev_of(b.facts.mac);
    check(!b.facts.mac.empty() && !b.facts.ip.empty() && b.veth != "NONE", label + ": guest eth0 " + b.facts.ip + " " + b.facts.mac + " behind " + b.veth);
    return b;
}

std::string bexec_text(KataBackend& be, Box& b, std::string const& source, EffectContext& ctx, std::string* code = nullptr) {
    ExecRequest r;
    r.source = source;
    auto o = agentengine::rt::block_on(be.exec(b.handle, r, ctx));
    if (!o) {
        if (code != nullptr) *code = o.error().code;
        return "EXEC-ERROR " + o.error().code + ": " + o.error().message;
    }
    if (code != nullptr) code->clear();
    return o->stdout_text;
}

void destroy_box(KataBackend& be, Box& b) { agentengine::rt::block_on(be.destroy(b.handle)); }

struct Host {
    std::string lan_ip, v6ll, gw_mac;
};

std::string std_probes(std::string const& id, Host const& h, std::string const& peer_ip = "10.88.77.250") {
    return guest_py(id, probe_script({{"w9000", "198.51.100.2", "9000"}, {"w9001", "198.51.100.2", "9001"}, {"w2_9000", "198.51.100.3", "9000"},
                                       {"gw22", "10.88.77.1", "22"}, {"lan22", h.lan_ip, "22"}, {"v6", h.v6ll, "22"}, {"peer", peer_ip, "7777"}})).out;
}

constexpr char kListener[] =
    "import socket\ns=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', 7777)); s.listen(8)\n"
    "while True:\n    c,_=s.accept(); c.sendall(b'PEER-OK\\n'); c.close()\n";

void start_listener(std::string const& id) { (void)guest_exec(id, "(nohup python3 -c " + shq(kListener) + " >/dev/null 2>&1 &) ; sleep 1"); }

bool starts(std::string const& s, char const* p) { return s.rfind(p, 0) == 0; }

// Everything a create()/destroy() pair must leave unchanged.
struct World {
    std::string ports, netns, containers, tables, workdirs, nft_files;
    bool operator==(World const&) const = default;
};
World take_world() {
    World w;
    w.ports = bridge_ports();
    w.netns = netns_list();
    w.containers = ctr_containers();
    w.tables = ae_tables();
    w.workdirs = trim(sh("ls /run/agentengine-kata 2>/dev/null | grep -v '^egress$' | sort | tr '\\n' ' '").out);
    w.nft_files = trim(sh("ls /run/agentengine-kata/egress 2>/dev/null | grep -c '\\.nft$\\|\\.batch$'").out);
    return w;
}
std::string describe(World const& w) {
    return "ports[" + w.ports + "] netns[" + w.netns + "] containers[" + w.containers + "] tables[" + w.tables + "] workdirs[" + w.workdirs + "] files[" + w.nft_files + "]";
}

std::string table_listing() { return sh("nft list table bridge ae_k").out; }

// A fake nft/tc/bridge first on PATH that fails when AEPV_FAIL_TOOL matches and the argument line contains
// AEPV_FAIL_PATTERN, and otherwise runs the real tool. run_ctr() resolves argv[0] through the environment PATH.
struct FaultInjection {
    std::string old_path;
    FaultInjection() {
        (void)sh("rm -rf /tmp/aepv-fake && mkdir -p /tmp/aepv-fake");
        for (char const* tool : {"nft", "tc", "bridge"}) {
            std::string const real = trim(sh(std::string("command -v ") + tool).out);
            std::string const script = std::string("#!/bin/sh\nif [ \"$AEPV_FAIL_TOOL\" = \"") + tool +
                                       "\" ] && echo \"$*\" | grep -q -- \"$AEPV_FAIL_PATTERN\"; then echo 'aepv: injected failure' >&2; exit 1; fi\nexec " + real + " \"$@\"\n";
            std::ofstream("/tmp/aepv-fake/" + std::string(tool)) << script;
            (void)sh(std::string("chmod +x /tmp/aepv-fake/") + tool);
        }
        char const* p = std::getenv("PATH");
        old_path = p != nullptr ? p : "";
        ::setenv("PATH", ("/tmp/aepv-fake:" + old_path).c_str(), 1);
    }
    static void fail(char const* tool, char const* pattern) {
        ::setenv("AEPV_FAIL_TOOL", tool, 1);
        ::setenv("AEPV_FAIL_PATTERN", pattern, 1);
    }
    static void heal() { ::unsetenv("AEPV_FAIL_TOOL"); }
    ~FaultInjection() {
        ::setenv("PATH", old_path.c_str(), 1);
        heal();
        (void)sh("rm -rf /tmp/aepv-fake");
    }
};

// A lab port on the Kata bridge with no VM behind it (real veth pair, real bridge port), for HostEgress-level
// tests that must run many installs or need a port to delete.
struct LabPort {
    std::string veth, peer, mac, ip;
    int n = 0;
    bool create(int idx, std::string const& bridge = "aekata0") {
        n = idx;
        veth = "aepvs" + std::to_string(idx);
        peer = "aepvt" + std::to_string(idx);
        char buf[32];
        std::snprintf(buf, sizeof buf, "02:aa:00:00:%02x:%02x", idx / 256, idx % 256);
        mac = buf;
        ip = "10.88.77." + std::to_string(200 + idx);
        return sh("ip link add " + veth + " type veth peer name " + peer + " && ip link set " + veth + " master " + bridge + " && ip link set " + veth + " up && ip link set " + peer + " up").ok();
    }
    void destroy() { (void)sh("ip link delete " + veth); }
    [[nodiscard]] egress::SandboxPlan plan(std::string const& bridge = "aekata0") const {
        egress::SandboxPlan p;
        p.tag = "aepv-lab" + std::to_string(n);
        p.veth = veth;
        p.bridge = bridge;
        p.guest_mac = mac;
        p.guest_ip = (10u << 24) | (88u << 16) | (77u << 8) | static_cast<unsigned>(200 + n);
        p.gateway_ip = (10u << 24) | (88u << 16) | (77u << 8) | 1u;
        p.allowed = {{(198u << 24) | (51u << 16) | (100u << 8) | 2u, 9000}};
        return p;
    }
};

}  // namespace

int main() {
    EffectContext ctx = make_ctx();
    if (::geteuid() != 0) {
        std::fprintf(stderr, "FAIL: these tests need root\n");
        return 1;
    }
    Lab lab;
    check(lab.up(), "lab up");
    if (g_failures != 0) return 1;
    check(Lab::forward_precondition_met(), "PRECONDITION (operator): FORWARD accepts for aekata0 <-> aepvw0 (ae-prove-temp)");
    World const baseline = take_world();
    check(baseline.tables == "0", "baseline: no ae_k table");
    Host host;
    host.lan_ip = trim(sh("ip -4 route get 1.1.1.1 | sed -n 's/.* src \\([0-9.]*\\).*/\\1/p' | head -1").out);
    host.v6ll = trim(sh("ip -6 -o addr show dev aekata0 scope link | sed -n 's/.*inet6 \\(fe80[0-9a-f:]*\\)\\/.*/\\1/p' | head -1").out);
    host.gw_mac = host_mac("aekata0");

    // ---- control for every B1 variant: the unpoliced world (no table, no tc) opens everything ------------------
    std::string ctl_probes;
    {
        ControlSandbox c1, c2;
        if (c1.create("aepv-lc1") && c2.create("aepv-lc2")) {
            start_listener(c2.id);
            ctl_probes = guest_py(c1.id, probe_script({{"gw22", "10.88.77.1", "22"}, {"w2_9000", "198.51.100.3", "9000"}, {"w9001", "198.51.100.2", "9001"}, {"peer", c2.ip, "7777"}})).out;
            check(parse_probe(ctl_probes, "gw22").find("SSH-") != std::string::npos && parse_probe(ctl_probes, "w2_9000").find("WORLD-OK") != std::string::npos &&
                      parse_probe(ctl_probes, "peer").find("PEER-OK") != std::string::npos,
                  "CONTROL (pre-change behaviour): with no table and no tc the guest reaches host sshd, an unlisted IP and a peer guest");
        } else {
            check(false, "control sandboxes boot");
        }
        c1.destroy();
        c2.destroy();
    }

    // ============ T1: the whole shared table is deleted (what `nft flush ruleset` does to it) =================
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s1 = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "T1/S1");
        auto s2 = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "T1/S2 (peer)");
        if (s1 && s2) {
            start_listener(s2->id());
            std::string const base_out = std_probes(s1->id(), host, s2->facts.ip);
            check(starts(parse_probe(base_out, "w9000"), "CONNECTED WORLD-OK") && starts(parse_probe(base_out, "gw22"), "FAILED"), "T1 baseline: allowed tuple works, host sshd blocked");

            check(sh("nft delete table bridge ae_k").ok(), "T1: nft delete table bridge ae_k (as a flush ruleset would)");
            check(ae_tables() == "0", "T1: the shared table is really gone");

            // the guest keeps running; probe it directly (bypassing the backend's own exec-time verification)
            std::string const out = std_probes(s1->id(), host, s2->facts.ip);
            check(starts(parse_probe(out, "w9000"), "CONNECTED WORLD-OK"), "T1: after table loss the allowed tuple still works (tc allows exactly it)");
            check(starts(parse_probe(out, "gw22"), "FAILED") && starts(parse_probe(out, "lan22"), "FAILED") && starts(parse_probe(out, "v6"), "FAILED"),
                  "T1: after table loss host sshd (gateway + LAN) and IPv6 are STILL blocked (" + parse_probe(out, "gw22") + ")");
            check(starts(parse_probe(out, "w9001"), "FAILED") && starts(parse_probe(out, "w2_9000"), "FAILED"), "T1: after table loss other port / unlisted IP are STILL blocked");
            check(starts(parse_probe(out, "peer"), "FAILED"), "T1: after table loss guest-to-guest is STILL blocked (isolated ports + tc)");

            AttackTarget t{s1->facts.mac, s1->facts.ip, s2->facts.mac, s2->facts.ip, host.gw_mac};
            auto counts = run_attack_batch(s1->id(), t);
            int leaked = 0;
            for (auto const& m : blocked_markers()) leaked += counts[m] != 0 ? 1 : 0;
            check(leaked == 0 && counts["AEPV-LEGIT"] > 0 && counts["AEPV-ARPOK"] > 0,
                  "T1: after table loss the hostile frame batch is STILL blocked by tc + port flags (" + counts_line(counts) + ")");

            // positive control: remove the tc layer too -> the same probes now succeed
            check(sh("tc qdisc del dev " + s1->veth + " clsact").ok(), "T1 control: remove the tc layer from the veth host end");
            std::string const open_out = std_probes(s1->id(), host, s2->facts.ip);
            check(starts(parse_probe(open_out, "gw22"), "CONNECTED SSH-") && starts(parse_probe(open_out, "w2_9000"), "CONNECTED WORLD-OK"),
                  "T1 CONTROL: with the table AND tc gone the guest reaches host sshd and an unlisted IP -- the probes can see an open path (" + parse_probe(open_out, "gw22") + ")");
            evidence("table loss (nft delete table / flush ruleset): guest still contained", "same probes with tc also removed reach host sshd and unlisted IP; unpoliced control too",
                     parse_probe(out, "gw22") + " | " + parse_probe(out, "w2_9000") + " | " + parse_probe(out, "peer") + " | batch " + counts_line(counts));

            // the backend notices on the next exec, kills the guest and refuses
            std::string code;
            std::string const e1 = bexec_text(backend, *s1, "echo should-not-run", ctx, &code);
            check(code == "kata_backend.egress_policy_missing" && e1.find("should-not-run") == std::string::npos, "T1: exec() after table loss is refused with kata_backend.egress_policy_missing (" + code + ")");
            std::string const ctr_state = sh("ctr task ls | grep '" + s1->id() + "'").out;
            check(ctr_state.find("RUNNING") == std::string::npos, "T1: the guest task was killed (" + trim(ctr_state) + ")");
            std::string const e2 = bexec_text(backend, *s1, "echo again", ctx, &code);
            check(code == "kata_backend.egress_policy_missing", "T1: every later exec() is refused too");
            evidence("exec-time verification detects table loss, kills the guest, refuses", "an untouched sandbox's exec runs (T1 baseline probes)", code);
        }
        if (s1) destroy_box(backend, *s1);
        if (s2) destroy_box(backend, *s2);
    }
    check(take_world() == baseline, "T1: destroy after table loss leaves the world as it was: " + describe(take_world()));

    // ============ T2: the tc layer is removed; nft alone must contain, then nft is deleted too ================
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "T2");
        if (s) {
            check(sh("tc qdisc del dev " + s->veth + " clsact").ok(), "T2: remove the tc layer (nft + port flags remain)");
            std::string const out = std_probes(s->id(), host);
            check(starts(parse_probe(out, "w9000"), "CONNECTED WORLD-OK") && starts(parse_probe(out, "gw22"), "FAILED") && starts(parse_probe(out, "lan22"), "FAILED") &&
                      starts(parse_probe(out, "v6"), "FAILED") && starts(parse_probe(out, "w9001"), "FAILED") && starts(parse_probe(out, "w2_9000"), "FAILED"),
                  "T2: nft layer alone enforces the exact allowlist and blocks host sshd/IPv6");
            AttackTarget t{s->facts.mac, s->facts.ip, "02:bb:00:00:00:09", "10.88.77.249", host.gw_mac};
            auto counts = run_attack_batch(s->id(), t);
            int leaked = 0;
            for (auto const& m : blocked_markers()) leaked += counts[m] != 0 ? 1 : 0;
            check(leaked == 0 && counts["AEPV-LEGIT"] > 0, "T2: nft layer alone blocks the hostile frame batch (" + counts_line(counts) + ")");
            check(sh("nft delete table bridge ae_k").ok(), "T2 control: now delete the table as well");
            std::string const open_out = std_probes(s->id(), host);
            check(starts(parse_probe(open_out, "gw22"), "CONNECTED SSH-"), "T2 CONTROL: with both layers gone host sshd is reachable -- nft was the only barrier (" + parse_probe(open_out, "gw22") + ")");
            evidence("tc removed: nft + port flags alone contain the guest", "both layers removed -> host sshd reachable", parse_probe(out, "gw22") + " | batch " + counts_line(counts));
            std::string code;
            (void)bexec_text(backend, *s, "echo x", ctx, &code);
            check(code == "kata_backend.egress_policy_missing", "T2: exec() refused after layers were removed");
        }
        if (s) destroy_box(backend, *s);
    }
    check(take_world() == baseline, "T2: world restored");

    // ============ T3: a rule is inserted into the sandbox's input chain (tamper) ==============================
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "T3");
        if (s) {
            std::string code;
            std::string const ok_exec = bexec_text(backend, *s, "echo untampered-ok", ctx, &code);
            check(code.empty() && ok_exec.find("untampered-ok") != std::string::npos, "T3 control: an untampered sandbox's exec() runs");
            // nft AND tc: remove tc so the tampered nft chain is the only layer deciding
            check(sh("tc qdisc del dev " + s->veth + " clsact").ok(), "T3: (tc layer removed so the tampered chain alone decides)");
            check(sh("nft insert rule bridge ae_k in_" + s->id() + " accept && nft insert rule bridge ae_k out_" + s->id() + " accept").ok(),
                  "T3: insert an unconditional accept at the top of the sandbox's input AND output chains");
            std::string const out = std_probes(s->id(), host);
            check(starts(parse_probe(out, "gw22"), "CONNECTED SSH-"), "T3 CONTROL: the tampered chain really opens the host (probe sees it: " + parse_probe(out, "gw22") + ")");
            (void)bexec_text(backend, *s, "echo x", ctx, &code);
            check(code == "kata_backend.egress_policy_missing", "T3: exec() detects the altered chain (byte-compare against the install-time snapshot) and refuses");
            evidence("tampered sandbox chain is detected at exec", "untampered exec runs; the tamper opens host sshd", code);
        }
        if (s) destroy_box(backend, *s);
    }
    check(take_world() == baseline, "T3: world restored");

    // ============ T4: bridge port flags reset ================================================================
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "T4");
        if (s) {
            std::string const flags_before = sh("bridge -d link show dev " + s->veth).out;
            check(flags_before.find("learning off") != std::string::npos && flags_before.find("locked on") != std::string::npos && flags_before.find("isolated on") != std::string::npos,
                  "T4: the port is created with learning off, locked on, isolated on");
            check(sh("bridge link set dev " + s->veth + " learning on locked off isolated off").ok(), "T4: reset the flags");
            std::string code;
            (void)bexec_text(backend, *s, "echo x", ctx, &code);
            check(code == "kata_backend.egress_policy_missing", "T4: exec() detects the reset port flags and refuses");
        }
        if (s) destroy_box(backend, *s);
    }
    check(take_world() == baseline, "T4: world restored");

    // ============ T5: teardown cleanliness + deny_all shape ===================================================
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s = make_box(backend, make_spec({"198.51.100.2:9000:http"}, 100000), ctx, "T5");
        if (s) {
            std::string const l = table_listing();
            check(l.find("chain in_" + s->id()) != std::string::npos && l.find("chain out_" + s->id()) != std::string::npos && l.find("quota q_" + s->id()) != std::string::npos &&
                      l.find("\"" + s->veth + "\" : jump in_" + s->id()) != std::string::npos,
                  "T5: while alive the table holds the sandbox's chains, quota and vmap entries");
            destroy_box(backend, *s);
            check(take_world() == baseline, "T5: destroy leaves zero tables/map entries/ports/netns/containers/workdirs/files: " + describe(take_world()));
            evidence("teardown leaves nothing", "during life the table, chains, quota and vmap entries exist", describe(take_world()));
        }
        SandboxSpec deny;  // default NetPolicy: deny_all, no allowlist
        auto d = agentengine::rt::block_on(backend.create(deny, ctx));
        check(d.has_value(), "T5: deny_all create()");
        if (d) {
            std::string const nics = trim(guest_exec(d->opaque_id, "ls /sys/class/net | tr '\\n' ' '").out);
            check(nics == "lo", "T5: deny_all guest has only lo (" + nics + ")");
            check(ae_tables() == "0" && bridge_ports() == baseline.ports && netns_list() == baseline.netns, "T5: deny_all creates no netns, no bridge port, no table");
            agentengine::rt::block_on(backend.destroy(*d));
        }
    }

    // ============ T6: two sandboxes created, probed and destroyed in parallel =================================
    {
        KataBackend backend(kRuntime, kPythonImage);  // ONE backend shared by both threads
        std::barrier sync(2);
        struct Res {
            std::string probes;
            std::string id;
            bool created = false;
        } ra, rb;
        auto work = [&](Res& r, char const* entry, std::string const& label) {
            EffectContext tctx = make_ctx();
            auto h = agentengine::rt::block_on(backend.create(make_spec({entry}), tctx));
            r.created = h.has_value();
            sync.arrive_and_wait();  // both exist now
            if (h) {
                r.id = h->opaque_id;
                r.probes = std_probes(h->opaque_id, host);
            }
            sync.arrive_and_wait();  // both probed
            if (h) agentengine::rt::block_on(backend.destroy(*h));
            (void)label;
        };
        std::thread ta(work, std::ref(ra), "198.51.100.2:9000:http", "A");
        std::thread tb(work, std::ref(rb), "198.51.100.3:9000:http", "B");
        ta.join();
        tb.join();
        check(ra.created && rb.created, "T6: both parallel create() calls succeeded");
        check(starts(parse_probe(ra.probes, "w9000"), "CONNECTED WORLD-OK 198.51.100.2") && starts(parse_probe(ra.probes, "w2_9000"), "FAILED") && starts(parse_probe(ra.probes, "gw22"), "FAILED"),
              "T6: sandbox A (allows .2:9000) reaches only its own tuple (" + parse_probe(ra.probes, "w9000") + " / " + parse_probe(ra.probes, "w2_9000") + ")");
        check(starts(parse_probe(rb.probes, "w2_9000"), "CONNECTED WORLD-OK 198.51.100.3") && starts(parse_probe(rb.probes, "w9000"), "FAILED") && starts(parse_probe(rb.probes, "gw22"), "FAILED"),
              "T6: sandbox B (allows .3:9000) reaches only its own tuple (" + parse_probe(rb.probes, "w2_9000") + " / " + parse_probe(rb.probes, "w9000") + ")");
        check(take_world() == baseline, "T6: parallel destroy left nothing behind: " + describe(take_world()));
        evidence("parallel create/probe/destroy of two sandboxes (one shared backend)", "each sandbox reaches its own tuple and not the other's", describe(take_world()));
    }

    // ============ T7: create failure paths (injected) leave nothing behind ==================================
    {
        FaultInjection fi;
        KataBackend backend(kRuntime, kPythonImage);
        struct Point {
            char const* name;
            char const* tool;
            char const* pattern;
        };
        for (Point const& p : {Point{"bridge link set (port flags)", "bridge", "link set"}, Point{"bridge fdb replace (static FDB)", "bridge", "fdb replace"},
                                Point{"tc -batch (tc layer)", "tc", "-batch"}, Point{"nft -f base table", "nft", "/base.nft"},
                                Point{"nft -f sandbox transaction", "nft", "/sb-"}, Point{"nft list table (read-back/ensure)", "nft", "list table"}}) {
            World const before = take_world();
            FaultInjection::fail(p.tool, p.pattern);
            auto h = agentengine::rt::block_on(backend.create(make_spec({"198.51.100.2:9000:http"}), ctx));
            FaultInjection::heal();
            check(!h.has_value() && h.error().code == "kata_backend.egress_policy_install_failed",
                  std::string("T7 inject ") + p.name + ": create() fails closed with egress_policy_install_failed (" + (h ? std::string("UNEXPECTED SUCCESS") : h.error().code) + ")");
            if (h) agentengine::rt::block_on(backend.destroy(*h));
            check(take_world() == before, std::string("T7 inject ") + p.name + ": nothing left behind: " + describe(take_world()));
        }
        // control: the identical create without injection succeeds and DOES hold resources until destroy
        World const before = take_world();
        auto ok = agentengine::rt::block_on(backend.create(make_spec({"198.51.100.2:9000:http"}), ctx));
        check(ok.has_value(), "T7 CONTROL: the same create() with no injected failure succeeds");
        if (ok) {
            check(!(take_world() == before), "T7 CONTROL: a successful create holds a port/netns/container/table (so the clean results above are not vacuous)");
            agentengine::rt::block_on(backend.destroy(*ok));
        }
        check(take_world() == before, "T7: control destroyed cleanly");

        // failure AFTER the policy is installed: `ctr run` fails (a one-byte memory limit) -> policy removed too
        SandboxSpec bad = make_spec({"198.51.100.2:9000:http"});
        bad.limits.memory_bytes = 1;
        auto h = agentengine::rt::block_on(backend.create(bad, ctx));
        check(!h.has_value() && h.error().code == "kata_backend.create_failed", "T7: ctr run failure after policy install -> create_failed (" + (h ? std::string("UNEXPECTED") : h.error().code) + ")");
        if (h) agentengine::rt::block_on(backend.destroy(*h));
        World const after_bad = take_world();
        check(after_bad.tables == "0" && after_bad.ports == before.ports && after_bad.netns == before.netns && after_bad.containers == before.containers,
              "T7: ctr run failure after install removes the policy, port, netns and container: " + describe(after_bad));
    }

    // ============ T8: sweep keys on the port, never on a netns path; remove() never drops a live port's policy ==
    {
        KataBackend backend(kRuntime, kPythonImage);
        auto s = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "T8");
        if (s) {
            egress::HostEgress he(real_runner());
            LabPort stale;
            check(stale.create(1), "T8: lab port aepvs1 on the Kata bridge (a stale entry to be swept)");
            auto installed = he.install(stale.plan());
            check(installed.has_value(), "T8: install() on the lab port" + (installed ? std::string() : " -- " + installed.error().message));
            check(table_listing().find("jump in_aepv-lab1") != std::string::npos, "T8: the stale entry is in the table");

            // the netns path of the LIVE sandbox disappears (cleanup script, tmpfiles, a second backend...)
            check(sh("ip netns delete " + s->id()).ok(), "T8: `ip netns delete` of the live sandbox's netns (the guest keeps running)");
            check(sh("ls /var/run/netns | grep -c '^" + s->id() + "$'").out.find('0') == 0, "T8: its /var/run/netns path is gone");

            // remove() of the stale entry while its port still exists is refused (the policy outlives the port)
            auto early = he.remove("aepv-lab1", "aepvs1");
            check(!early.has_value() && early.error().code == "kata_backend.egress_policy_kept", "T8: remove() of a still-present port's policy is refused (kept)");
            check(table_listing().find("jump in_aepv-lab1") != std::string::npos, "T8 control: the entry is still there after the refused remove");

            stale.destroy();  // the port disappears
            auto sw = he.sweep_orphans();
            check(sw.has_value(), "T8: sweep_orphans() ran");
            std::string const l = table_listing();
            check(l.find("jump in_aepv-lab1") == std::string::npos && l.find("chain in_aepv-lab1") == std::string::npos, "T8 CONTROL: the sweep DOES remove the entry whose port is gone (the sweep can act)");
            check(l.find("jump in_" + s->id()) != std::string::npos && l.find("chain out_" + s->id()) != std::string::npos,
                  "T8: the sweep did NOT touch the live sandbox whose netns path was deleted");
            std::string const out = std_probes(s->id(), host);
            check(starts(parse_probe(out, "w9000"), "CONNECTED WORLD-OK") && starts(parse_probe(out, "gw22"), "FAILED"), "T8: the live sandbox still works and is still contained after the sweep");
            std::string code;
            (void)bexec_text(backend, *s, "echo still-ok", ctx, &code);
            check(code.empty(), "T8: exec() of the live sandbox still verifies after the sweep");
            evidence("sweep keys on the bridge port, not on /var/run/netns; a live sandbox's entry survives `ip netns del`", "the entry of a deleted port IS swept in the same run", "live entry intact, stale entry removed");
            // destroy: `ip netns delete` already ran, so destroy's own netns delete reports failure (logged), fine
        }
        if (s) destroy_box(backend, *s);
        check(take_world() == baseline, "T8: world restored after destroy: " + describe(take_world()));
    }

    // ============ T9: destroy whose policy removal fails keeps the policy; the next sweep removes it ==========
    {
        FaultInjection fi;
        KataBackend backend(kRuntime, kPythonImage);
        auto s = make_box(backend, make_spec({"198.51.100.2:9000:http"}), ctx, "T9");
        if (s) {
            FaultInjection::fail("nft", "/rm-");
            destroy_box(backend, *s);
            FaultInjection::heal();
            std::string const l = table_listing();
            check(l.find("jump in_" + s->id()) != std::string::npos, "T9: when the policy removal fails the entry is KEPT (fail closed), the port is already gone");
            check(!egress::HostEgress::port_exists(s->veth), "T9: the veth is gone");
            egress::HostEgress he(real_runner());
            check(he.sweep_orphans().has_value() && ae_tables() == "0", "T9: the next sweep removes the stale entry and the empty table");
        }
        check(take_world() == baseline, "T9: world restored: " + describe(take_world()));
    }

    // ============ T10: idempotent install (no duplicate rules) =================================================
    {
        egress::HostEgress he(real_runner());
        LabPort p1, p2;
        check(p1.create(2) && p2.create(3), "T10: two lab ports");
        auto i1 = he.install(p1.plan());
        check(i1.has_value(), "T10: first install");
        std::string const base_before = table_listing();
        auto dup = he.install(p1.plan());  // same plan again
        check(!dup.has_value(), "T10: installing the SAME sandbox twice is refused, not merged");
        check(table_listing() == base_before, "T10: the refused duplicate install changed NOTHING (no duplicate rules, original intact)");
        auto i2 = he.install(p2.plan());
        check(i2.has_value(), "T10: second (different) sandbox installs");
        std::string const after = table_listing();
        for (char const* b : {"chain c_pre", "chain c_input", "chain c_forward", "chain c_output"}) {
            check(egress::extract_block(after, b) == egress::extract_block(base_before, b), std::string("T10: base ") + b + " is byte-identical after a second sandbox installs (not re-appended)");
        }
        check(i1 && he.verify(*i1).has_value(), "T10: the first sandbox still verifies after the second one's install");
        // control: the naive way (re-loading the base script over the existing table) DOES duplicate rules
        std::ofstream("/tmp/aepv-naive.nft") << egress::render_base_table();
        check(sh("nft -f /tmp/aepv-naive.nft").ok(), "T10 CONTROL: re-loading the base table text over the existing table");
        check(!egress::base_table_matches(table_listing()), "T10 CONTROL: ...appends duplicate rules (base_table_matches now false), the failure mode idempotent install avoids");
        (void)sh("rm -f /tmp/aepv-naive.nft");
        (void)sh("nft delete table bridge ae_k");
        p1.destroy();
        p2.destroy();
        (void)he.sweep_orphans();
    }
    check(take_world() == baseline, "T10: world restored");

    // ============ T11: A-record cap and host-local rejection ================================================
    {
        // temporary /etc/hosts block (removed below): 12 records for one name, 3 for another
        std::string block;
        for (int i = 10; i < 22; ++i) block += "198.51.100." + std::to_string(i) + " aepv-multi.test # aepv-temp\n";
        for (int i = 30; i < 33; ++i) block += "198.51.100." + std::to_string(i) + " aepv-three.test # aepv-temp\n";
        { std::ofstream h("/etc/hosts", std::ios::app); h << block; }
        {
            KataBackend backend(kRuntime, kPythonImage);
            auto many = make_box(backend, make_spec({"aepv-multi.test:9000:http"}), ctx, "T11 (12 A records)");
            auto three = make_box(backend, make_spec({"aepv-three.test:9000:http"}), ctx, "T11 control (3 A records)");
            auto count_rules = [&](Box& b) { return first_int_after(sh("nft list chain bridge ae_k in_" + b.id() + " | grep -c 'tcp dport 9000 accept'").out, ""); };
            if (many) check(count_rules(*many) == static_cast<long long>(egress::kMaxEndpointsPerEntry), "T11: 12 A records are capped to 8 allowed tuples (" + std::to_string(count_rules(*many)) + ")");
            if (three) check(count_rules(*three) == 3, "T11 CONTROL: a 3-record name allows ALL 3 (not just the pinned first): " + std::to_string(count_rules(*three)));
            if (many) {
                std::string const hosts = trim(guest_exec(many->id(), "grep multi /etc/hosts").out);
                check(hosts == "198.51.100.10 aepv-multi.test", "T11: the guest /etc/hosts pins exactly the first record (" + hosts + ")");
            }
            if (many) destroy_box(backend, *many);
            if (three) destroy_box(backend, *three);

            // host-local entry: 198.51.100.1 is the host's own address on aepvw0 -> rejected before any resource
            World const before = take_world();
            auto local = agentengine::rt::block_on(backend.create(make_spec({"198.51.100.1:9000:http"}), ctx));
            check(!local.has_value() && local.error().code == "kata_backend.allowlist_entry_blocked", "T11: an entry that is an address of this host is rejected (" + (local ? std::string("UNEXPECTED") : local.error().code) + ")");
            check(take_world() == before, "T11: ...before any resource was acquired");
            auto fine = agentengine::rt::block_on(backend.create(make_spec({"198.51.100.2:9000:http"}), ctx));
            check(fine.has_value(), "T11 CONTROL: the equally-shaped non-host-local entry is accepted");
            if (fine) agentengine::rt::block_on(backend.destroy(*fine));
            if (local) agentengine::rt::block_on(backend.destroy(*local));
        }
        (void)sh("sed -i '/# aepv-temp$/d' /etc/hosts");
        check(sh("grep -c aepv-temp /etc/hosts").out.find('0') == 0, "T11: the temporary /etc/hosts block is removed");
    }
    check(take_world() == baseline, "T11: world restored");

    lab.down();
    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_backend_egress_lifecycle_linux: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_backend_egress_lifecycle_linux: %d FAILURE(S)\n", g_failures.load());
    return 1;
}
