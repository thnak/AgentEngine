#pragma once
// KataBackend egress policy: the enforcement point for NetPolicy::allowlist and ResourceLimits::net_bytes
// of a Kata microVM guest (ADR-240; 008 §4, I2, I3, I8; design -> red-team -> prove record in
// docs/planning/2026-10-19-kata-egress-policy-{design,redteam}.md, revised design in redteam §6).
//
// Why here and not in the sandbox netns (ADR-093/095's `ip netns exec <id> nft ... ae_netpolicy`): Kata
// joins the tap and the veth with tc mirred redirects, so the guest's frames never traverse that netns's
// netfilter hooks (live finding 2, 2026-10-19). The guest's frames DO cross the host end of the veth, a port of
// the CNI bridge in the host namespace. The policy therefore lives on that port, in three layers that fail
// independently:
//
//   1. native bridge-port hardening (`learning off`, `locked on`, `isolated on`, a static FDB entry): closes
//      source-MAC spoofing, FDB poisoning and guest-to-guest at L2 and survives any nft flush;
//   2. a `tc clsact` allowlist on the veth host end (default-deny first): the exact (guest mac/ip, d, tcp p)
//      tuples, exact ARP to the gateway, no fragments; survives any nft flush;
//   3. ONE shared nftables `bridge` table (`ae_k`) with verdict-map dispatch: default drop for any port of the
//      CNI bridge that has no map entry, per-sandbox chains with ARP/SYN-rate/quota/tuple/ct rules, early
//      fragment drop. Carries the byte quota and conntrack statefulness, which layers 1-2 cannot.
//
// Every value emitted to nft/tc/bridge is typed (uint32 address, uint16 port, uint64 bytes) or validated by
// this header's own charset checks (MAC, interface name, tag): no string from a guest, a DNS answer or model
// output reaches a generated command line (I3). The allowed tuples are the backend's own resolution of
// spec.net.allowlist entries that authorize_spec() already required to be covered by a
// cap::SandboxNetOut grant (I2); this module only ever ADDS drops, it never edits iptables/ufw/Docker, never
// opens FORWARD and never touches a sysctl.
//
// The command runner is injected, so every failure path is testable without a host (egress_policy_render.cpp is
// pure; egress_policy_host.cpp shells out through the runner).

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::kata::egress {

inline constexpr std::string_view kTableName = "ae_k";
inline constexpr std::string_view kTableComment = "agentengine-kata-egress-v1";
inline constexpr std::size_t kMaxEndpointsPerEntry = 8;  // A-record cap per allowlist entry (redteam R19)
inline constexpr std::size_t kMaxEndpointsTotal = 64;
inline constexpr unsigned kSynRatePerSecond = 100;  // redteam R12: bounds host conntrack growth per sandbox
inline constexpr unsigned kSynBurstPackets = 50;

struct Endpoint {
    std::uint32_t ipv4_host_order = 0;
    std::uint16_t port = 0;
    friend bool operator==(Endpoint const&, Endpoint const&) = default;
};

// One sandbox's policy inputs. `tag` is the backend id (also the nft chain/object suffix).
struct SandboxPlan {
    std::string tag;
    std::string veth;       // host end of the veth pair, a port of `bridge`
    std::string bridge;     // the CNI bridge the veth is enslaved to
    std::string guest_mac;  // lowercase colon-hex, unicast
    std::uint32_t guest_ip = 0;
    std::uint32_t gateway_ip = 0;
    std::vector<Endpoint> allowed;  // non-empty: deny-all sandboxes have no NIC and no plan
    std::uint64_t net_bytes = 0;    // 0 = no quota
};

[[nodiscard]] bool valid_tag(std::string_view s);
[[nodiscard]] bool valid_ifname(std::string_view s);
[[nodiscard]] bool valid_mac(std::string_view s);  // lowercase colon-hex, unicast, not all-zero
[[nodiscard]] std::string ipv4_dotted(std::uint32_t host_order);
[[nodiscard]] result<std::monostate> validate(SandboxPlan const& plan);

// Pure generators. Each validates its inputs first and refuses (error) rather than emitting anything odd.
[[nodiscard]] std::string render_base_table();
[[nodiscard]] result<std::string> render_install_script(SandboxPlan const& plan);
[[nodiscard]] result<std::string> render_remove_script(std::string_view tag, std::string_view veth);
[[nodiscard]] result<std::string> render_tc_batch(SandboxPlan const& plan);

// Listing parsers used for read-back, exec-time verification and the sweep (pure, tested on canned text).
[[nodiscard]] std::string extract_block(std::string_view listing, std::string_view header);
[[nodiscard]] std::size_t count_lines(std::string_view block);
struct MapElement {
    std::string veth;
    std::string chain;  // e.g. "in_ae-kata-1a"
};
[[nodiscard]] std::vector<MapElement> parse_map_elements(std::string_view map_listing);
[[nodiscard]] bool base_table_matches(std::string_view table_listing);

// `cnitool add` result JSON (CNI spec 1.x): the guest's MAC and address, the gateway, and the host-side
// candidates (interfaces without `sandbox`: the veth and the bridge; host code picks the one that is a port).
struct CniAttachment {
    std::string guest_mac;
    std::uint32_t guest_ip = 0;
    std::uint32_t gateway_ip = 0;
    std::vector<std::string> host_interface_candidates;
};
[[nodiscard]] result<CniAttachment> parse_cni_result(std::string_view json_text);

// ---- host effects --------------------------------------------------------------------------------------------

struct CommandOutcome {
    int exit_code = -1;
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
};
using CommandRunner = std::function<result<CommandOutcome>(std::vector<std::string> const&)>;

// What install() proved and snapshotted; exec() re-checks against it.
struct InstalledPolicy {
    SandboxPlan plan;
    std::string nft_in_chain;    // `nft list table` block of in_<tag>, right after install
    std::string nft_out_chain;
    std::string tc_ingress;      // `tc filter show dev <veth> ingress`
    std::string tc_egress;
};

class HostEgress {
public:
    explicit HostEgress(CommandRunner runner, std::string state_dir = "/run/agentengine-kata/egress");

    // Resolves which candidate interface is a bridge port and returns the plan skeleton fields
    // (veth, bridge). Fails `kata_backend.egress_topology_unsupported` when none/ambiguous or the master is not
    // a Linux bridge.
    [[nodiscard]] result<std::pair<std::string, std::string>> locate_port(
        std::vector<std::string> const& candidates) const;

    // Under the advisory lock: sweep orphans, harden the port, tc, shared table ensure + sandbox transaction,
    // read-back and snapshot. On any failure the partial state of THIS sandbox is removed before returning.
    [[nodiscard]] result<InstalledPolicy> install(SandboxPlan const& plan);

    // Re-verifies everything install() established. Error code kata_backend.egress_policy_missing.
    [[nodiscard]] result<std::monostate> verify(InstalledPolicy const& policy);

    // Remove this sandbox's nft state (idempotent) and delete the table when no port remains. The caller must
    // already have deleted the veth: when the port still exists the policy is KEPT and an error is returned.
    [[nodiscard]] result<std::monostate> remove(std::string_view tag, std::string_view veth);

    // Remove entries whose port no longer exists (keyed on the port, never on a netns path).
    [[nodiscard]] result<std::monostate> sweep_orphans();

    [[nodiscard]] static bool port_exists(std::string_view veth);

private:
    struct Lock;
    [[nodiscard]] result<Lock> acquire_lock();
    [[nodiscard]] result<std::monostate> verify_impl(InstalledPolicy const& policy, std::string const* preloaded_listing);
    [[nodiscard]] result<std::monostate> run_ok(std::vector<std::string> const& argv, char const* what);
    [[nodiscard]] result<std::string> run_text(std::vector<std::string> const& argv, char const* what);
    [[nodiscard]] result<std::monostate> load_nft(std::string const& script, std::string_view file_stem);
    [[nodiscard]] result<std::monostate> ensure_base_locked();
    [[nodiscard]] result<std::monostate> remove_locked(std::string_view tag, std::string_view veth);
    [[nodiscard]] result<std::monostate> sweep_locked();
    [[nodiscard]] bool port_set_empty(std::string_view table_listing) const;

    CommandRunner runner_;
    std::string state_dir_;
};

}  // namespace agentengine::kata::egress
