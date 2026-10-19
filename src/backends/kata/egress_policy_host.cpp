// Host-effect half of the Kata egress policy (ADR-239): runs bridge/tc/nft through an injected runner, under an
// advisory flock so concurrent creates/destroys serialise across threads and processes. See egress_policy.hpp's
// top comment for the design record; this file owns the fail-closed ordering:
//   create:  sweep orphans -> port flags + static FDB -> tc (default-deny first) -> shared table ensure + one
//            sandbox transaction -> read back + snapshot; any failure removes this sandbox's partial state.
//   destroy: the port must already be gone (the policy outlives the port); only then nft state is removed.
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "backends/kata/egress_policy.hpp"

namespace agentengine::kata::egress {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] error fail(failure_class k, std::string message, char const* code) {
    return error{k, std::move(message), code};
}

[[nodiscard]] error install_error(std::string message) {
    return fail(failure_class::fatal, "egress: " + std::move(message), "kata_backend.egress_policy_install_failed");
}

[[nodiscard]] error missing_error(std::string message) {
    return fail(failure_class::fatal, "egress policy verification failed: " + std::move(message), "kata_backend.egress_policy_missing");
}

[[nodiscard]] std::string sysfs(std::string_view veth, char const* leaf = "") {
    return "/sys/class/net/" + std::string(veth) + (leaf[0] != '\0' ? std::string("/") + leaf : std::string());
}

[[nodiscard]] bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

[[nodiscard]] std::vector<std::string> quoted_names(std::string_view block) {
    std::vector<std::string> out;
    std::size_t at = block.find("elements = {");
    if (at == std::string_view::npos) return out;
    std::size_t const end = block.find('}', at);
    std::size_t pos = at;
    while (true) {
        std::size_t const open = block.find('"', pos);
        if (open == std::string_view::npos || open > end) break;
        std::size_t const close = block.find('"', open + 1);
        if (close == std::string_view::npos || close > end) break;
        out.emplace_back(block.substr(open + 1, close - open - 1));
        pos = close + 1;
    }
    return out;
}

}  // namespace

struct HostEgress::Lock {
    int fd = -1;
    Lock() = default;
    Lock(Lock const&) = delete;
    Lock& operator=(Lock const&) = delete;
    Lock(Lock&& o) noexcept : fd(o.fd) { o.fd = -1; }
    Lock& operator=(Lock&&) = delete;
    ~Lock() {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    }
};

HostEgress::HostEgress(CommandRunner runner, std::string state_dir) : runner_(std::move(runner)), state_dir_(std::move(state_dir)) {}

bool HostEgress::port_exists(std::string_view veth) {
    std::error_code ec;
    return fs::exists(sysfs(veth), ec);
}

result<std::monostate> HostEgress::run_ok(std::vector<std::string> const& argv, char const* what) {
    auto r = runner_(argv);
    if (!r) return std::unexpected(install_error(std::string(what) + ": " + r.error().message));
    if (r->timed_out || r->exit_code != 0) {
        return std::unexpected(install_error(std::string(what) + " failed (exit " + std::to_string(r->exit_code) + "): " + r->stderr_text));
    }
    return std::monostate{};
}

result<std::string> HostEgress::run_text(std::vector<std::string> const& argv, char const* what) {
    auto r = runner_(argv);
    if (!r) return std::unexpected(install_error(std::string(what) + ": " + r.error().message));
    if (r->timed_out || r->exit_code != 0) {
        return std::unexpected(install_error(std::string(what) + " failed (exit " + std::to_string(r->exit_code) + "): " + r->stderr_text));
    }
    return std::move(r->stdout_text);
}

result<std::monostate> HostEgress::load_nft(std::string const& script, std::string_view stem) {
    std::string const path = state_dir_ + "/" + std::string(stem) + ".nft";
    {
        std::ofstream f(path, std::ios::trunc);
        f << script;
        if (!f) return std::unexpected(install_error("cannot write " + path));
    }
    auto r = run_ok({"nft", "-f", path}, "nft -f");
    std::error_code ec;
    fs::remove(path, ec);
    return r;
}

result<std::pair<std::string, std::string>> HostEgress::locate_port(std::vector<std::string> const& candidates) const {
    auto const unsupported = [](std::string why) {
        return std::unexpected(fail(failure_class::policy, "egress: CNI topology not supported: " + std::move(why), "kata_backend.egress_topology_unsupported"));
    };
    std::string veth;
    std::string bridge;
    for (std::string const& c : candidates) {
        if (!valid_ifname(c)) continue;
        std::error_code ec;
        fs::path const master = fs::read_symlink(sysfs(c, "master"), ec);
        if (ec) continue;  // no master: the bridge itself, or not enslaved
        if (!veth.empty()) return unsupported("more than one bridge port reported by the CNI plugin");
        veth = c;
        bridge = master.filename().string();
    }
    if (veth.empty()) return unsupported("no host interface reported by the CNI plugin is a bridge port (a routed/ptp topology?)");
    if (!valid_ifname(bridge)) return unsupported("bridge name is not a valid interface name");
    std::error_code ec;
    if (!fs::exists(sysfs(bridge, "bridge"), ec)) return unsupported("the port's master is not a Linux bridge");
    return std::make_pair(std::move(veth), std::move(bridge));
}

result<std::monostate> HostEgress::ensure_base_locked() {
    auto listing = runner_({"nft", "list", "table", "bridge", std::string(kTableName)});
    if (listing && !listing->timed_out && listing->exit_code == 0) {
        if (base_table_matches(listing->stdout_text)) return std::monostate{};
        if (port_set_empty(listing->stdout_text)) {
            // no registered port, so no live sandbox of ours depends on the stale/foreign table: replace it
            if (auto d = run_ok({"nft", "delete", "table", "bridge", std::string(kTableName)}, "nft delete table"); !d) return d;
        } else {
            return std::unexpected(install_error("an existing nftables table 'bridge ae_k' does not match this backend's base table while "
                                                 "sandboxes are registered; refusing to run with a policy I did not install"));
        }
    } else if (!listing) {
        return std::unexpected(install_error("nft is not runnable: " + listing.error().message));
    } else if (!contains(listing->stderr_text, "No such file or directory")) {
        // not "the table does not exist": a transient or foreign failure. Creating the base table over a table
        // that DOES exist would append duplicate rules to its chains, so refuse instead of guessing.
        return std::unexpected(install_error("nft list table bridge ae_k failed for a reason other than absence (exit " +
                                             std::to_string(listing->exit_code) + "): " + listing->stderr_text));
    }
    return load_nft(render_base_table(), "base");
}

bool HostEgress::port_set_empty(std::string_view table_listing) const {
    return quoted_names(extract_block(table_listing, "set ports")).empty();
}

result<std::monostate> HostEgress::sweep_locked() {
    auto listing = runner_({"nft", "list", "table", "bridge", std::string(kTableName)});
    if (!listing || listing->timed_out || listing->exit_code != 0) return std::monostate{};  // no table: nothing to sweep
    std::string const& l = listing->stdout_text;

    std::vector<MapElement> const in_elems = parse_map_elements(extract_block(l, "map in_d"));
    std::vector<std::string> const ports = quoted_names(extract_block(l, "set ports"));
    std::string script;
    std::size_t remaining_ports = 0;
    auto const live = [](std::string const& veth) { return HostEgress::port_exists(veth); };
    for (std::string const& p : ports) {
        if (live(p)) ++remaining_ports;
    }
    auto const referenced = [&](std::string const& tag) {
        return std::any_of(in_elems.begin(), in_elems.end(), [&](MapElement const& e) { return e.chain == "in_" + tag; });
    };
    // 1. map entries whose port is gone (keyed on the port, never on a netns path)
    for (MapElement const& e : in_elems) {
        if (live(e.veth) || e.chain.rfind("in_", 0) != 0) continue;
        auto s = render_remove_script(e.chain.substr(3), e.veth);
        if (s) script += *s;
    }
    // 2. ports-set members with no live port
    for (std::string const& p : ports) {
        if (!live(p) && valid_ifname(p)) script += "destroy element bridge " + std::string(kTableName) + " ports { \"" + p + "\" }\n";
    }
    // 3. per-sandbox chains no map element refers to (garbage; install is one transaction so this is not a race)
    std::size_t pos = 0;
    while ((pos = l.find("\tchain in_", pos)) != std::string::npos) {
        std::size_t const name_begin = pos + std::string_view("\tchain in_").size();
        std::size_t const name_end = l.find(' ', name_begin);
        std::string const tag = l.substr(name_begin, name_end - name_begin);
        pos = name_end;
        if (valid_tag(tag) && !referenced(tag)) {
            auto s = render_remove_script(tag, "orphan0");  // veth part destroys a non-existent element: a no-op
            if (s) script += *s;
        }
    }
    if (!script.empty()) {
        if (auto r = load_nft(script, "sweep"); !r) return r;
    }
    // last registered port gone: remove the whole table so the bridge is no longer default-dropped
    if (remaining_ports == 0) {
        auto again = runner_({"nft", "list", "table", "bridge", std::string(kTableName)});
        if (again && again->exit_code == 0 && port_set_empty(again->stdout_text)) {
            if (auto d = run_ok({"nft", "delete", "table", "bridge", std::string(kTableName)}, "nft delete table"); !d) return d;
        }
    }
    return std::monostate{};
}

result<HostEgress::Lock> HostEgress::acquire_lock() {
    std::error_code ec;
    fs::create_directories(state_dir_, ec);
    ::chmod(state_dir_.c_str(), 0700);
    Lock l;
    l.fd = ::open((state_dir_ + "/lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (l.fd < 0) return std::unexpected(install_error("cannot open the egress lock file"));
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (::flock(l.fd, LOCK_EX | LOCK_NB) != 0) {
        if (std::chrono::steady_clock::now() > deadline) return std::unexpected(install_error("timed out waiting for the egress lock"));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return l;
}

result<std::monostate> HostEgress::sweep_orphans() {
    auto lock = acquire_lock();
    if (!lock) return std::unexpected(lock.error());
    return sweep_locked();
}

result<InstalledPolicy> HostEgress::install(SandboxPlan const& plan) {
    if (auto v = validate(plan); !v) return std::unexpected(v.error());
    // everything that can be computed without touching the host is computed before taking the lock
    auto tc_batch = render_tc_batch(plan);
    if (!tc_batch) return std::unexpected(tc_batch.error());
    auto nft_script = render_install_script(plan);
    if (!nft_script) return std::unexpected(nft_script.error());

    auto lock = acquire_lock();
    if (!lock) return std::unexpected(lock.error());

    (void)sweep_locked();  // failure to sweep must not block this sandbox; the next call retries

    // 1. native port hardening: closes spoofing/FDB poisoning/guest-to-guest at L2, survives any nft flush
    if (auto r = run_ok({"bridge", "link", "set", "dev", plan.veth, "learning", "off", "locked", "on", "isolated", "on"}, "bridge link set"); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = run_ok({"bridge", "fdb", "replace", plan.guest_mac, "dev", plan.veth, "master", "static"}, "bridge fdb replace"); !r) {
        return std::unexpected(r.error());
    }
    // 2. tc allowlist on the host end (default-deny first inside the batch)
    {
        std::string const path = state_dir_ + "/tc-" + plan.tag + ".batch";
        {
            std::ofstream f(path, std::ios::trunc);
            f << *tc_batch;
            if (!f) return std::unexpected(install_error("cannot write " + path));
        }
        auto r = run_ok({"tc", "-batch", path}, "tc -batch");
        std::error_code rm_ec;
        fs::remove(path, rm_ec);
        if (!r) return std::unexpected(r.error());
    }
    // 3. shared table + this sandbox's transaction
    // `undo` only runs after OUR transaction committed: a transaction that failed created nothing, and undoing it
    // would delete another install's identically named objects (a duplicate install must change nothing).
    auto const undo = [&]() { (void)remove_locked(plan.tag, plan.veth); };
    if (auto r = ensure_base_locked(); !r) return std::unexpected(r.error());
    if (auto r = load_nft(*nft_script, "sb-" + plan.tag); !r) return std::unexpected(r.error());
    // 4. read back and snapshot
    InstalledPolicy snapshot;
    snapshot.plan = plan;
    auto listing = run_text({"nft", "list", "table", "bridge", std::string(kTableName)}, "nft list table");
    auto ing = run_text({"tc", "filter", "show", "dev", plan.veth, "ingress"}, "tc filter show");
    auto egr = run_text({"tc", "filter", "show", "dev", plan.veth, "egress"}, "tc filter show");
    if (!listing || !ing || !egr) {
        undo();
        return std::unexpected(!listing ? listing.error() : (!ing ? ing.error() : egr.error()));
    }
    snapshot.nft_in_chain = extract_block(*listing, "chain in_" + plan.tag);
    snapshot.nft_out_chain = extract_block(*listing, "chain out_" + plan.tag);
    snapshot.tc_ingress = std::move(*ing);
    snapshot.tc_egress = std::move(*egr);
    if (snapshot.nft_in_chain.empty() || snapshot.nft_out_chain.empty()) {
        undo();
        return std::unexpected(install_error("read-back did not find this sandbox's chains"));
    }
    if (auto v = verify(snapshot); !v) {
        undo();
        return std::unexpected(install_error("read-back verification failed: " + v.error().message));
    }
    return snapshot;
}

result<std::monostate> HostEgress::verify(InstalledPolicy const& pol) {
    SandboxPlan const& p = pol.plan;
    if (!port_exists(p.veth)) return std::unexpected(missing_error("the sandbox's bridge port no longer exists"));

    auto link = runner_({"bridge", "-d", "link", "show", "dev", p.veth});
    if (!link || link->exit_code != 0) return std::unexpected(missing_error("cannot read the bridge port flags"));
    for (char const* flag : {"learning off", "locked on", "isolated on"}) {
        if (!contains(link->stdout_text, flag)) return std::unexpected(missing_error(std::string("bridge port flag '") + flag + "' is not set"));
    }
    auto fdb = runner_({"bridge", "fdb", "show", "dev", p.veth});
    if (!fdb || fdb->exit_code != 0) return std::unexpected(missing_error("cannot read the port's FDB"));
    {
        bool found = false;
        std::string const& t = fdb->stdout_text;
        std::size_t pos = 0;
        while (pos < t.size()) {
            std::size_t eol = t.find('\n', pos);
            if (eol == std::string::npos) eol = t.size();
            std::string_view line(t.data() + pos, eol - pos);
            if (contains(line, p.guest_mac) && contains(line, "static") && !contains(line, "self")) found = true;
            pos = eol + 1;
        }
        if (!found) return std::unexpected(missing_error("the static FDB entry for the guest MAC is missing"));
    }

    auto listing = runner_({"nft", "list", "table", "bridge", std::string(kTableName)});
    if (!listing || listing->timed_out || listing->exit_code != 0) return std::unexpected(missing_error("the shared nftables table bridge ae_k is gone"));
    std::string const& l = listing->stdout_text;
    if (!base_table_matches(l)) return std::unexpected(missing_error("the shared table's base chains are not what this backend installed"));
    if (extract_block(l, "chain in_" + p.tag) != pol.nft_in_chain) return std::unexpected(missing_error("this sandbox's input chain changed or is gone"));
    if (extract_block(l, "chain out_" + p.tag) != pol.nft_out_chain) return std::unexpected(missing_error("this sandbox's output chain changed or is gone"));
    auto const has_elem = [&](char const* map, std::string const& chain) {
        for (MapElement const& e : parse_map_elements(extract_block(l, map))) {
            if (e.veth == p.veth && e.chain == chain) return true;
        }
        return false;
    };
    if (!has_elem("map in_d", "in_" + p.tag) || !has_elem("map out_d", "out_" + p.tag)) {
        return std::unexpected(missing_error("this sandbox's dispatch entries are missing from the verdict maps"));
    }
    {
        auto const ports = quoted_names(extract_block(l, "set ports"));
        auto const bridges = quoted_names(extract_block(l, "set bridges"));
        if (std::find(ports.begin(), ports.end(), p.veth) == ports.end() || std::find(bridges.begin(), bridges.end(), p.bridge) == bridges.end()) {
            return std::unexpected(missing_error("this sandbox's port or bridge is missing from the shared sets"));
        }
    }
    if (extract_block(l, "counter d_" + p.tag).empty()) return std::unexpected(missing_error("the sandbox's drop counter is gone"));
    if (p.net_bytes > 0 && extract_block(l, "quota q_" + p.tag).empty()) {
        return std::unexpected(missing_error("the sandbox's net_bytes quota object is gone"));
    }

    auto ing = runner_({"tc", "filter", "show", "dev", p.veth, "ingress"});
    auto egr = runner_({"tc", "filter", "show", "dev", p.veth, "egress"});
    if (!ing || !egr || ing->exit_code != 0 || egr->exit_code != 0) return std::unexpected(missing_error("cannot read the tc filters"));
    if (ing->stdout_text != pol.tc_ingress || egr->stdout_text != pol.tc_egress) {
        return std::unexpected(missing_error("the tc allowlist on the veth host end changed or is gone"));
    }
    return std::monostate{};
}

result<std::monostate> HostEgress::remove_locked(std::string_view tag, std::string_view veth) {
    auto script = render_remove_script(tag, veth);
    if (!script) return std::unexpected(script.error());
    auto listing = runner_({"nft", "list", "table", "bridge", std::string(kTableName)});
    if (!listing || listing->timed_out || listing->exit_code != 0) return std::monostate{};  // no table: nothing of ours remains
    if (auto r = load_nft(*script, "rm-" + std::string(tag)); !r) return r;
    return sweep_locked();  // also deletes the table when no registered port remains
}

result<std::monostate> HostEgress::remove(std::string_view tag, std::string_view veth) {
    if (!valid_tag(tag) || !valid_ifname(veth)) {
        return std::unexpected(install_error("invalid tag or interface name for removal"));
    }
    if (port_exists(veth)) {
        // fail-open guard: never drop the policy of a port that still exists (the guest may still run)
        return std::unexpected(fail(failure_class::fatal,
                                    "egress: the sandbox's bridge port still exists; its policy is KEPT (a later sweep removes it once the port is gone)",
                                    "kata_backend.egress_policy_kept"));
    }
    auto lock = acquire_lock();
    if (!lock) return std::unexpected(lock.error());
    return remove_locked(tag, veth);
}

}  // namespace agentengine::kata::egress
