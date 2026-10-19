# KataBackend egress policy: design draft (issue #194)

Status: DESIGN stage of design -> red-team -> prove -> judge. Nothing here is implemented; no source or
test was changed. Evidence base: `docs/planning/2026-10-19-kata-live-verification-findings.md` (Finding 2).
Date: 2026-10-19. Host: `thnak-server` (Ubuntu 26.04, kernel 7.0.0-38, nftables 1.1.6, Kata 4.1.0
runtime-rs + cloud-hypervisor, CNI bridge `ae-kata-net` / `aekata0` 10.88.77.0/24, host `FORWARD` policy
DROP via Docker + ufw, `br_netfilter` not loaded: `bridge-nf-call-iptables` absent).

Every claim carries a tag. **MEASURED** = run on the server (command and result summarised, scripts were
under `/tmp/ae-design/` on the server and have been cleaned up; the reference generator is reproduced in
Appendix A). **REASONED** = argued, not run; the red-team/prove stages should treat these as the
weakest claims.

## 0. Summary

* The existing `ae_netpolicy` nft table sees none of the guest's traffic (MEASURED), and a *second*
  natural fix, nft `netdev` hooks on the netns's `eth0`/`tap0_kata`, is **also bypassed** (MEASURED:
  counters 0 while the guest connected freely) because Kata joins the tap and the veth with `tc mirred`
  redirects, which run before / skip the netfilter ingress/egress hooks.
* A guest can reach the host itself (INPUT path), other guests (pure L2, never touches IP netfilter) and
  host-link-local IPv6 services, none of which any current rule covers (all MEASURED).
* **Recommendation: a per-sandbox nftables `bridge`-family table in the host namespace, keyed on the
  sandbox's bridge port (the veth host end)**, loaded atomically after `cnitool add` and before the VM
  boots, hooks `input`/`forward`/`output` at priority -300, default-drop for that port, exact
  `(guest ip/mac, dest ip, tcp port)` allow rules, strict ARP rules, a shared `quota` for `net_bytes`,
  deleted on destroy and swept when orphaned. Measured end to end including the internet path, spoofing,
  guest-to-guest, IPv6 and exact bidirectional byte counting.
* The backend must **never** modify the host's iptables/ufw/Docker rules. Whether the guest can reach
  the internet at all on a host with `FORWARD DROP` remains an operator precondition (MEASURED: needed a
  temporary scoped ACCEPT). The backend's job is the *restriction*, which fails closed independent of that.

## 1. Measured facts (the evidence the design rests on)

All guest probes: `sudo ctr tasks exec --exec-id X <cid> sh -c ...` into a microVM started the way the
backend does it by hand (`ip netns add`, `cnitool add ae-kata-net`, `ctr run -d --runtime
io.containerd.kata-clh.v2 --with-ns network:/var/run/netns/<id> busybox sleep`). Guest = busybox 1.38
(nc, wget, arping; no curl). Guest caps as the backend grants them: includes `CAP_NET_RAW`, not
`CAP_NET_ADMIN` (`kata_backend_detail.hpp` lines 366-368).

| # | Fact | Tag | How |
|---|---|---|---|
| M1 | Baseline (existing backend wiring, no filter, `FORWARD DROP`): guest reaches host sshd on gateway 10.88.77.1:22 and host LAN IP 192.168.1.7:22 (banner read); internet unreachable | MEASURED | `/tmp/probe.sh` via exec: `E`/`F` lines returned `SSH-2.0-OpenSSH_10.2p1`, `A-D,G` rc=1 |
| M2 | Guest-to-guest on the shared bridge is open: guest A `nc` to guest B `10.88.77.35:7777` delivered the payload; ping works | MEASURED | two microVMs aed1/aed2 |
| M3 | IPv6 link-local to the host is open: `nc fe80::d8f0:...%eth0 22` read the host sshd banner; `ping -6` ok | MEASURED | same |
| M4 | Guest never touches the netns stack: Kata installs `tc filter ... u32 match all action mirred egress redirect` on **both** `eth0` ingress (-> `tap0_kata`) and `tap0_kata` ingress (-> `eth0`) at pref 49152 | MEASURED | `tc filter show dev {eth0,tap0_kata} ingress` |
| M5 | **Candidate A is dead:** nft `netdev` table with `hook ingress`+`hook egress device "eth0"`, policy drop, allowing only one tuple: the guest still reached `10.88.77.1:22`, a second guest and the host sshd over IPv6; all counters 0. Only the netns's own stack traffic (one 70-byte IPv6 packet) hit `g2w_drop`. Mechanism REASONED: `tc` ingress (mirred, "stolen") runs before the nf ingress hook and redirected skbs skip the nf egress hook | MEASURED (effect) / REASONED (kernel mechanism) | `ip netns exec aed1 nft -f n1.nft`; probe |
| M6 | **Candidate B works:** host-namespace `table bridge` with `input`/`forward`/`output` hooks (priority -300) keyed `iifname`/`oifname <veth host end>`, without `br_netfilter`: unlisted gw:22, guest-to-guest, IPv6 (nc and ping6) all blocked; the single allowlisted tuple worked; counters incremented | MEASURED | Appendix A ruleset, same probes |
| M7 | B with the internet path (temporary `iptables -I FORWARD 1 -i/-o aekata0 -j ACCEPT` carrying comment `ae-design-temp`, removed by `trap` in the same command, count verified 0): without filter 1.1.1.1:443, 8.8.8.8:53, 104.20.23.154:443, 1.1.1.1:80 all rc=0; with B allowing only `1.1.1.1:443`: that rc=0, the other three rc=1 (same IP different port blocked) | MEASURED | `inet.sh` |
| M8 | B byte counting, both directions, stateless counters at L3 length: download 1 MiB counted `w2g` 1,050,196 B; upload 1 MiB counted `g2w` ~1,068,036 B (about 1.8% header overhead); 3 further identical runs all moved exactly 1,048,576 B. (One first run returned 0 downloaded bytes with no explanation found; 4 later runs fine. Flag for the prove stage: flake root cause unknown.) | MEASURED | `bytes.sh` against a host python sink, temporary INPUT accept for 9998/9999 with `ae-design-temp`, removed in the same command |
| M9 | B shared `quota budget { over 200000 bytes }` placed before the accept rules on input and output: guest downloaded 196,792 B then was cut; the quota showed `used 200000 bytes`; the following upload got rc=1 (budget is shared across directions) | MEASURED | `genb.sh ... 200000` |
| M10 | B anti-spoofing from a guest with `CAP_NET_ADMIN` (test-only): after adding IP .200 and sending from it to gw:22 -> blocked; from .200 to the allowed destination -> 0 bytes; after changing the MAC to 02:aa:.. then allowed flow -> 0 bytes and gw:22 blocked; baseline allowed flow 210,184 B | MEASURED | `spoof2.sh` |
| M11 | **Candidate D (the "obvious" fix) is unsound:** `inet` table `input` hook keyed `ip saddr <guest ip>` drop blocked gw:22, but guest-to-guest was untouched (forward counter 0, bridged, no netfilter), and after `ip addr add 10.88.77.200` + `nc -s 10.88.77.200` the guest reached gw:22 again | MEASURED | `ae_d_temp` table (deleted) |
| M12 | Routed attachment (candidate E): CNI `ptp` in a temporary conf dir (`/tmp`, no host persistent change, `ipMasq:false`) boots Kata; host veth is a plain routed interface with `/32` route. Unfiltered the guest reaches the host LAN IP, the host's *other* network's gateway `10.88.77.1:22` and its own gateway; guest-to-guest was blocked only because host `FORWARD` is DROP. With `inet` table `input`/`forward` keyed `iifname <veth>` the host-local probes were blocked (drop counters 6 and 3 packets) | MEASURED | `ae-ptp.conflist` |
| M13 | tc approach (candidate C): flower rules at pref 10/11 on `tap0_kata` and `eth0` ingress with `mirred redirect` + matchall drop at pref 100 enforced the allowlist, blocked gw:22, guest-to-guest, IPv6; per-filter byte/drop counters exist | MEASURED | `tc1.sh` |
| M14 | `tc police` cannot be an exact byte quota: `police rate 8bit burst 200000b` is rejected "burst out of range" (burst is bounded in time units, so a large burst needs a rate that refills the bucket) | MEASURED (rejection) / REASONED (leak consequence) | `tc1.sh ... 200000` |
| M15 | Kata and pre-installed qdiscs: `tc qdisc add dev eth0 clsact` before boot makes the shim fail (`add virt ingress ... File exists`); a pre-installed **root `prio`** qdisc + matchall drop on `eth0` is tolerated, and its egress filter **does** see the redirected guest->world packets (14 dropped, guest `nc` failed) | MEASURED | fresh netns aed4 |
| M16 | `deny_all` (empty netns, no CNI): the guest has only `lo` | MEASURED | netns without CNI |
| M17 | `example.com` has 2 A and 2 AAAA records (104.20.23.154, 172.66.147.243; order differs per resolver); `resolve_and_validate` returns one | MEASURED (DNS) / REASONED (function returns one: `ipv4_host_order`, kata_backend.cpp:207-216) | `getent ahosts`, `dig @1.1.1.1/@8.8.8.8` |
| M18 | The guest has no `/etc/resolv.conf` in the hand-wired run (the backend only writes `/etc/hosts`) | MEASURED | `cat` in guest |
| M19 | Kata runtime-rs leaves stale sockets in `/run/containerd/s` after a failed `ctr run` and a failed run leaves a stale `tap0_kata` in the netns (second attempt: `unsupported link type: tuntap`); the same container id hashes to the same socket | MEASURED | aed4 retries |
| M20 | `busybox arping` cannot forge a foreign sender (`bind: Cannot assign requested address`), so ARP-poisoning resistance was **not** exercised | MEASURED (tool limit) | arping |

Server hygiene at the end: 0 `ae-design-temp` rules in iptables and nft, all `aed*` tasks, containers,
netns, veths removed, `cnitest-*` namespaces deleted, no `ae_*` nft tables, my stale socket removed
(one older stale socket `DF0036F5...` from 14:09 is not mine and was left). Note the coordinator's own
cleanup removed `cnitest-*` namespaces mid-run; my guests survived it (verified alive afterwards).

## 2. Baseline packet paths (current backend)

Topology: guest `eth0` (virtio-net) <-> cloud-hypervisor tap `tap0_kata` (in netns) <-(tc mirred both
ways)-> netns `eth0` (veth) <-> host veth `vethXXXX` (bridge port of `aekata0`) <-> `aekata0` (10.88.77.1)
<-> host IP stack <-> `enp7s0`.

| Flow | Path today | Backend rule hit |
|---|---|---|
| guest -> internet | frame to gw MAC: tap -> mirred -> netns eth0 -> veth -> bridge `aekata0` -> **local delivery on the bridge device** -> IP forward -> `FORWARD` (Docker/ufw policy DROP) -> masquerade (CNI `ipMasq`) -> `enp7s0` | none (netns `output` hook never sees mirred traffic) |
| guest -> host-local (10.88.77.1, 192.168.1.7, any host IP) | same, delivered locally: `INPUT`, source iface `aekata0` | none |
| guest -> other guest | bridge forwards L2 port-to-port; no IP netfilter (no `br_netfilter`) | none |
| host -> guest | host stack -> `aekata0` -> bridge FDB -> veth -> netns eth0 -> mirred -> tap | none |
| guest DNS | UDP/53 to the resolver IP in guest `resolv.conf` (none by default); `/etc/hosts` pin covers allowlisted names | none; DNS to any IP:53 follows the internet path |

## 3. Candidates

Common notes. *DNS and drift* apply to all candidates equally and are resolved once in section 4.4.
*Host firewall rule for all*: the backend only creates and deletes objects that it names itself
(`ae_k_<id>` tables or tc filters inside its own netns). It never edits `iptables`, `ufw`, Docker
chains, `ip_forward` or any sysctl, and never opens `FORWARD`. In nftables a verdict `accept` in one
base chain does not override a `drop` in another table's chain on the same hook, so no table of ours
could "open" a closed `FORWARD` anyway; ours can only add drops. Egress working at all on a
`FORWARD DROP` host is the operator's precondition (M7 needed a temporary rule).

### A. nft `netdev` table in the sandbox netns (hook on `eth0` / `tap0_kata`)
* Enforces nothing for guest traffic: **REJECTED by measurement (M5)**. tc mirred is first.
* Could it be fixed by deleting Kata's tc filters? That is a post-boot mutation of the shim's own
  datapath (racy, breaks on shim upgrade), and removes the only forwarding. Not pursued.

### B. nft `bridge`-family table in the host namespace keyed on the sandbox's bridge port (RECOMMENDED)
* Enforcement point: the bridge's own netfilter hooks (they exist without `br_netfilter`, M6).
  * guest -> internet / host-local: frame arrives at `aekata0` as bridge `input` (dst MAC = bridge)
    with `iifname = veth`; chain drops everything but allowlisted `(ip saddr=guest, ip daddr, tcp dport)`
    and the strict ARP to the gateway. The IP layer, `FORWARD` and masquerade run afterwards unchanged.
  * guest -> other guest: bridge `forward` with `iifname = veth`: dropped (M6).
  * other guest / host -> guest: `forward`/`output` with `oifname = veth`: only allowlisted source tuples
    and gateway ARP pass; this also stops other sandboxes talking to this one at the destination side.
  * guest -> DNS: dropped unless the destination ip:tcp port is allowlisted (UDP is never allowlisted).
  * IPv6, VLAN, STP, LLC: not matched by an accept, so dropped (M6, M3 closed).
* Cannot: control what the host IP stack does after `input` (that is `FORWARD`/`INPUT`/masquerade, which
  the operator owns); filter a topology that is not a Linux bridge (needs a topology check); count
  Ethernet header bytes (L3 length; M8 shows ~1.8% header overhead included for IP/TCP).
* Spoof resistance: keyed on the **port**, plus `ether saddr` and `ip saddr` equal to what CNI assigned
  (MEASURED against IP and MAC change, M10). ARP restricted to request/reply
  `sender = (guest mac, guest ip)`, `target = gateway` (REASONED for forged ARP; M20).
* Failure modes: `nft -f` is one kernel transaction, so a half-loaded table cannot exist. Failures
  before/at load -> `cleanup_partial()` unwinds `cnitool del` + `ip netns del` (existing code path) and
  `nft delete table` if the table was created. Crash between load and boot leaves an orphan table
  (harmless: it only drops traffic of a veth that no longer exists) -> startup sweep. **Table deleted
  externally while the guest runs fails OPEN** (REASONED): mitigated by verifying table presence on every
  `exec()` and refusing (and tearing down) if absent; hosts running `nft flush ruleset` are already
  outside the supported configuration.
* Docker/ufw interplay: separate family and table name; ufw/Docker manage `ip`/`ip6` tables only on this
  host (`nft list tables`: ip filter, ip6 filter, ip nat, ip6 nat, ip raw). If `br_netfilter` is loaded by
  some other component, bridged IP additionally traverses iptables (additive, cannot undo our drops).
* IPv6: dropped wholesale for the port (M6); no v6 support promised (the backend is v4-only today).
* Concurrency: one table per sandbox, `nft -f` serialised by the kernel; each bridged frame walks every
  sandbox's chain heads, a single `iifname` compare each (O(N) in sandboxes, REASONED; consider one
  shared table with an `iifname` verdict map if N reaches hundreds, a prove-stage measurement).
* Cost: ~one `nft -f` (a few ms, not timed), no new daemon, no new host dependency (nft already required
  by ADR-093). One new host dependency: the bridge CNI topology.

### C. tc filters inside the sandbox netns (flower + mirred/drop at the tap and veth)
* Enforcement: spoof-proof by device, lifecycle bound to the netns (dies with it); measured to enforce
  the same allowlist, gw, guest-guest and IPv6 cases (M13).
* Cannot: exact `net_bytes` (M14: `police` rejects large bursts; a feasible rate refills the bucket so
  the budget leaks `rate * lifetime`; a user-space watcher over `tc -s` counters is not real-time);
  tap0_kata exists only after the shim starts, so tap-side rules cannot be installed before the guest
  (race); eth0 can only be pre-guarded on **egress** with a root `prio` qdisc (M15) because Kata creates
  the ingress qdisc and a pre-existing `clsact` makes the shim fail; non-atomic (many `tc` invocations,
  half-done states need explicit teardown); ARP/IPv6 matching works but each rule needs two redirects.
* Interplay with Docker/ufw: none. Concurrency: per-netns, no shared state. Cost: many forks per create.
* Verdict: viable as defence in depth but loses on quota, atomicity and the post-boot race.

### D. nft `inet` table, `input` + `forward` hooks keyed on the guest's address (the issue's first idea)
* **REJECTED by measurement (M11).** Guest address is guest-controlled (IP change, raw `CAP_NET_RAW`
  sends); `forward` never sees bridged guest-to-guest; `iifname` is the bridge, not the port, so the key
  is the spoofable address. Also cannot cover IPv6 link-local without per-family rules and cannot see
  ARP.

### E. Routed attachment: CNI `ptp` (veth pair, no bridge) + host `inet` table keyed on `iifname veth`
* Enforcement: guest packets enter the host stack on a dedicated interface, so ordinary `input` and
  `forward` hooks (with `ct`, v4+v6 in one `inet` table) can key on `iifname`, spoof-proof (M12).
  Packet paths: guest->internet = `forward`; guest->host-local = `input`; guest->guest = `forward`
  (host-routed; unfiltered it is blocked only by the host's `FORWARD DROP`, M12, which is accidental);
  host->guest = `output`/`forward` with `oifname`; DNS as B.
* Cannot: nothing extra versus B; but it **changes the CNI topology** the runbook and ADR-093 document,
  makes `ipMasq`/host routes per sandbox (/32 routes accumulate: M12 showed `10.88.78.x dev veth... scope
  host`), and needs `ip_forward`. Stateful `ct` return-path matching is nicer than B's stateless
  `sport` match.
* Failure modes and Docker/ufw interplay as B (inet table named by us; an `accept` still cannot reopen a
  Docker DROP). Per-sandbox host routes are an extra leak class.
* Verdict: equal security to B at higher operational change. Keep as the documented alternative if an
  operator already runs a routed CNI; the generator for it is a small variation of B's (inet hooks,
  `iifname`, `ct state established,related`).

### F. Host-mediated egress (RFC 008 section 4: "egress is always host-mediated, no profile hands a guest a raw socket")
* Guest gets **no NIC** (M16 shows the empty-netns shape works); an allowlisted host proxy is reached
  over vsock/hybrid-vsock, e.g. `HostEgressProxy` (ADR-011) with a guest-side socat/shim.
* Strongest (no IP path exists, DNS/IPv6/ARP/spoofing/host-local are non-issues, byte counting is exact
  in the proxy) and the only candidate that matches RFC 008. Cannot: transparent for arbitrary guest
  programs (needs proxy-aware clients or a guest-side redirector), needs a vsock plumbing design for
  runtime-rs that nobody has built or measured here (REASONED only), HTTPS CONNECT/SNI semantics.
* Verdict: the right long-term direction and the honest reading of RFC 008, but a feature-sized project,
  not a fix to a security defect that is live today. Recorded as the follow-on; B is the interim that
  makes ADR-093's stated behaviour true. (Under ADR-070 an interim with disclosed residuals is the
  accepted pattern.)

### G. macvlan/ipvlan on the physical NIC (guest directly on the LAN)
* REASONED only. Bypasses the host stack, so `FORWARD`/`INPUT` do not matter and the host is
  unreachable from the child by macvlan design; but the guest sits on the physical LAN (all LAN hosts
  reachable at L2, broadcast, DHCP/ARP exposure), policy would have to live in tc in the netns (candidate
  C's limits), and it needs a physical-NIC dependency and per-host config. Rejected.

## 4. Recommendation: candidate B

### 4.1 Why the others lose
A: bypassed (M5). D: spoofable and blind to L2 (M11). C: no exact quota, tap race, non-atomic (M14,
M15). E: same security, bigger blast radius on operator topology (keep as alternative). F: correct
long-term, wrong size for an interim fix. G: wrong trust boundary. Only B has, together: one atomic
load, a pre-boot installation point (the veth host end exists after `cnitool add`, before `ctr run`),
port-keyed spoof resistance, exact shared byte quota, ARP/IPv6/L2 coverage, guest-to-guest isolation at
both ends, no change to the CNI config, and no change to the host firewall.

### 4.2 Policy semantics (what is enforced)
For one sandbox with CNI-assigned `(veth, mac, ip)`, gateway `gw`, validated endpoint set `S` of
`(ipv4, port)` and optional `net_bytes = N`:
* from the port (`input`): drop if `ether saddr != mac`; accept ARP request/reply only with
  `(sender mac, sender ip) = (mac, ip)` and target `gw`; if `N`: drop when the shared `quota` is `over N`;
  accept `ip saddr = ip, ip daddr = d, tcp dport = p` for `(d,p)` in `S`; drop everything else.
* from the bridge toward the port (`output`): ARP reply to `ip`, ARP request `gw -> ip`; if `N`: quota
  drop; accept `ip saddr = d, ip daddr = ip, tcp sport = p` for `(d,p)` in `S`; drop everything else.
* port-to-port (`forward`): drop both directions for the port.
* `deny_all` stays what it is today: netns with no interface (M16); no table is needed.
* Reference rule text with the final ARP rules is Appendix A.

### 4.3 Create / destroy sequence and fail-closed ordering
`validate (caps, allowlist parse, resolve) -> mount -> netns add -> cnitool add (parse result JSON) ->
resolve the host veth by peer ifindex and require it be a bridge port -> nft -f (atomic) -> read back the
table (exists, rule count) -> write OCI spec -> ctr run`. Any failure runs `cleanup_partial` in reverse
(`nft delete table` first if created). `destroy()`: kill task, delete task/container, **then**
`nft delete table`, then `cnitool del`, netns delete (the guest is gone before the policy is). Startup
sweep deletes bridge tables `ae_k_*` that have no `/var/run/netns/<id>`. `exec()` re-verifies the table
(cheap `nft list table`) and refuses otherwise.

### 4.4 DNS, hosts pinning, drift, IPv6
* Names resolve host-side only. Resolve **all** A records of an entry (each through the existing
  SSRF validation; M17: example.com has two), allow every validated `(ip,port)`, pin the first in the
  guest `/etc/hosts` (a name can map to several lines; glibc/musl use the first). The set is fixed for
  the sandbox lifetime; drift to an IP outside the set loses connectivity (availability only). A
  periodic host-side refresh that only *adds* freshly validated IPs for already-granted names is
  possible but rejected for v1 (it lets DNS answers, which are attacker-influenced data, widen the
  authority: an I3 smell). Unchanged ADR-093 posture, extended from one IP to the whole record set.
* Guest DNS to anything but a granted `ip:tcp-port` is dropped, so no DNS exfiltration channel and no
  resolver dependency; names absent from the pinned `/etc/hosts` simply fail.
* Reject an allowlist entry whose address is **local to the host** (any `getifaddrs` address or
  `ip route get` type `local`): such an entry would turn the allowlist into a grant over host services
  via the INPUT path; `resolve_and_validate` blocks RFC1918/loopback but not the host's own public IP.
* IPv6: no AAAA, no v6 rule; every IPv6 frame from the port is dropped (M3 closed by M6).

### 4.5 I2 / I3 / locked decisions
* I2: nothing new is reachable without `cap::SandboxNetOut` (the gate at kata_backend.cpp:157-186 is
  unchanged and still precedes any resource). The table is the *enforcement* of a granted capability, not
  a new authority; it adds only drops. The host firewall is untouched.
* I3: the rule inputs are the validated `spec.net.allowlist` (host/caller code), the CNI result
  (host-generated), and numbers. No model-derived string reaches `nft`: only IPv4 literals, ports,
  a validated MAC, a validated ifname and the sandbox id (charset-checked) are emitted. DNS answers are
  untrusted data constrained by `resolve_and_validate`; they select among IPs of an already granted name.
* ADR-070 seam: not needed (no delegation of authority to the host admin, except the documented
  `FORWARD` precondition which only governs whether egress works, not whether it is allowed).
* RFC 008 section 4's "egress is always host-mediated" is **not** met by B (the guest has a raw IP path to
  allowlisted endpoints). The ADR must say so and cite F as the converging direction (or amend 008 for
  the `kata` profile).

## 5. What changes in `src/backends/kata/kata_backend.cpp`

Stays: capability gate (lines ~157-186); allowlist grammar and `resolve_and_validate`; `/etc/hosts` pin;
netns + `cnitool add` + `cleanup_partial` structure; `deny_all` shape; `ctr run` and everything about disk,
pids, memory.

Changes:
1. **Remove** the `ip netns exec <id> nft ... ae_netpolicy` block (lines ~682-747: `output`/`input`
   chains, `ae_net_budget`, allow rules). Optionally keep only an `output` `policy drop` accepting `lo`
   for the netns's own processes (REASONED harmless: the existing allow rules never carried guest traffic,
   and the guest path worked with that table present, M1/M5), documented as defence in depth, never as the
   control.
2. **Add** `egress_policy.hpp/.cpp` (new, `kata_backend_detail`-style): pure
   `std::string render_bridge_policy(PolicyInputs)` (testable with no host), `install()` (`nft -f -`, then
   read-back), `remove()` (`nft delete table`), `sweep_orphans()`. Inputs are strongly typed (ipv4, port,
   mac, ifname), validated by the generator which refuses anything else.
3. Parse the `cnitool add` stdout (currently only the exit code is read) for the guest MAC/IP/gateway;
   find the host veth by `ip -o link` peer ifindex of the netns `eth0`; require `bridge_slave`/`master`
   (fail closed `kata_backend.egress_topology_unsupported`).
4. `resolve_and_validate` call: return/consume all A records (new overload in `net_egress_proxy`, no
   behavioural change to `HostEgressProxy`), plus the local-address rejection.
5. `net_bytes`: the quota object moves into the new table (input+output pair, shared). The `quota` rule
   keeps ordering before accepts (M9).
6. `destroy()`/`cleanup_partial()`: add `remove()` with the ordering of section 4.3; `exec()` verification.
7. New error codes: `kata_backend.egress_policy_install_failed`, `egress_topology_unsupported`,
   `egress_policy_missing`. The old `nft_setup_failed` remains for deprecation compatibility only.
8. Header comment of `kata_backend.hpp`: new precondition ("operator must permit `FORWARD` for the Kata
   bridge for guest internet egress; the backend neither opens nor requires changing the host firewall").

The ADR (next free number after ADR-238; Proposed, to go through the loop) must say, honestly:
* **ADR-093 section 3 and section 4 (last bullet) and ADR-095 section 3 ("net_bytes") are wrong for a
  microVM guest** as shipped: the netns `output`/`input` hooks never saw guest traffic (M4/M5), so the
  allowlist was not enforced, `net_bytes` counted ~0, and the live evidence is Finding 2. "Compile-
  verified only" was disclosed but the design claim was unsound, not just unverified.
* Open paths existed (M1-M3): host services, other sandboxes, IPv6 link-local.
* The new control (B) and its measured limits; the `FORWARD` precondition; the DNS-set pinning and drift
  residual; stateless return-path matching; flush-while-running fail-open and its mitigation; L3-length
  byte accounting; RFC 008 section 4 deviation and F as follow-on.
* Superseded text must be marked in ADR-093/095 headers (status line), not silently edited.

## 6. Acceptance tests (every claim -> a check that can fail)

Fixtures: **ControlSandbox** = netns + CNI + Kata with *no* policy (what M1-M3 used) so each negative
check has a positive control showing the path exists. **HostSink** = python TCP sink/source on the CI host
(allowed via a test-only `install(endpoints)` entry point since the backend refuses private/host-local
addresses), with the temporary `ae-design-temp`-style INPUT accept installed and removed by the harness.
Host `FORWARD` precondition for internet cases is set by the CI runbook (documented), never the backend.

| # | Claim | Test | Positive control that makes it able to fail |
|---|---|---|---|
| T1 | allowlisted dest reachable | slice9_10 case 6 allowed (`nc example.com 443`) CONNECTED | case 6 itself; and the table counter `g2w_bytes > 0` |
| T2 | unlisted IP blocked | case 6 blocked (`1.1.1.1:443`) FAILED **and** host `drops` counter grows | same probe from ControlSandbox with FORWARD open returns CONNECTED (M7) |
| T3 | port-level exactness | `1.1.1.1:80` FAILED when only `:443` allowed | `:443` CONNECTED in the same sandbox |
| T4 | DNS/UDP-53 blocked | `nslookup example.org 8.8.8.8` fails and nft counter on UDP/53 > 0 | ControlSandbox resolves; allowlisted pinned name resolves with no DNS packet |
| T5 | guest -> host-local blocked | `nc` to bridge gw `:22` and host LAN IP `:22` -> no banner | ControlSandbox reads `SSH-2.0` from both (M1) |
| T6 | guest -> other guest blocked | sandbox A `nc -l`, sandbox B connect + ping | two ControlSandboxes deliver payload (M2) |
| T7 | IPv6 blocked | `nc fe80::..%eth0 22`, `ping -6` | ControlSandbox banner/ping succeed (M3) |
| T8 | spoofing blocked | CAP_NET_ADMIN test guest (test-only spec): change IP, change MAC, then reach gw:22 and allowed dest | baseline allowed flow before the change returns bytes (M10); ControlSandbox spoofed flow reaches gw:22 (M11) |
| T9a | `net_bytes` counts inbound | allowed HostSink download, `N = 200000`: guest receives in `[0.9N, N]` | same download without `net_bytes` returns 1 MiB; nft `quota used` ~N |
| T9b | counts outbound | upload with `N`: sink receives `<= N` | upload without `net_bytes` delivers 1 MiB |
| T9c | shared bidirectional budget | upload 150 kB then download: total across both `<= N + 1 packet` | each direction alone can use the whole `N` in separate sandboxes |
| T9d | slice9_10 case 9 (256 B) cut off, 9b control passes | existing cases, unchanged | case 9b |
| T10 | fail closed on install failure | inject a failing `nft` (bad PATH) -> `create()` errors `egress_policy_install_failed`; afterwards no `/var/run/netns/<id>`, no `ae_k_*` table, no ctr container, no veth | immediately-following normal `create()` succeeds and leaves them present |
| T11 | unsupported topology fails closed | configure the backend with a `ptp` CNI network -> `egress_topology_unsupported`, nothing left behind | bridge network works |
| T12 | teardown + orphan sweep | after `destroy()` no `ae_k_*` table; simulate crash after install (kill harness before boot), next backend start sweeps it | table present before destroy / before sweep |
| T13 | externally removed table | delete the table while running; next `exec()` refuses and tears down | exec works with the table present |
| T14 | host-local allowlist entry rejected | entry resolving to a local host address -> `allowlist_entry_blocked` | an equally-shaped public entry accepted |
| T15 | multi-A allowlist | every validated A record of example.com reachable by IP literal; a third IP in the same /24 blocked | the pinned IP reachable |
| T16 | concurrency | 16 sandboxes in parallel with distinct allowlists: each reaches only its own set (matrix), none reaches another's, all tables gone after destroy | each sandbox reaches its own entry |
| T17 | host firewall untouched | `iptables-save` + `nft list ruleset` minus `ae_k_*` and CNI-plugin-owned rules identical before create and after destroy | snapshot function detects a deliberately added dummy rule |
| T18 | deny_all has no NIC | guest `/proc/net/dev` only `lo` | a sandbox with an allowlist has `eth0` (M16) |
| T19 | rule generation safety (I3) | table-driven unit test of `render_bridge_policy`: hostile strings (`; flush ruleset`, newlines, quotes) in every textual field are rejected, never emitted | well-formed inputs render exactly the golden text |
| T20 | capability gate unchanged | existing cases 2a-5 | existing cases |

## 7. Open questions for the red-team (attack these hardest)

1. **Fail-open on table loss** (4.3 / T13): exec-time verification only narrows the window. Is there a
   fail-closed-by-construction layout (shared base chain default-dropping all `aekata0` ports, with a
   per-sandbox allow set) that survives flush better, and is it worth losing per-sandbox atomic deletion?
2. **Bridge-family hook behaviour across kernels/br_netfilter states**: measured on one kernel with
   `br_netfilter` absent. What if `br_netfilter` is loaded, or an operator sets `bridge-nf-call-*`? What
   about `ebtables-legacy`, flow offload, or a bridge with VLAN filtering enabled (VLAN-tagged frames from
   the guest, QinQ)?
3. **Frames that match no rule type but get through**: `ether type ip` with IP options/fragments (first
   fragment carries the port, later fragments do not: do they pass `tcp dport` rules? they should not,
   check), IP-in-IP/GRE to an allowlisted IP, TCP to the allowed IP on an allowed port carrying a covert
   tunnel (inherent), allowed destination that is itself an open proxy/relay (inherent).
4. **ARP**: forged ARP from a `CAP_NET_RAW` guest could not be exercised (M20). Prove a raw sender cannot
   poison the gateway or another guest entry through the permitted ARP rules (gratuitous ARP with
   `sender ip = guest ip` for a victim target? the target is pinned to gw; unicast ARP to the bridge?).
5. **Stateless return path**: inbound frames from an allowlisted `ip:port` to the guest are accepted
   without `ct`. Can a remote or another sandbox spoof such a source to deliver unsolicited SYNs to a
   guest listener (the guest has no inbound allowlist)? Is a conntrack-based variant (E) required?
6. **The DNS-answer influence** (4.4): all-A-records expands the set to whatever the resolver returned at
   create time. Is that I3-clean when the name's owner (or a poisoned resolver) controls the set, versus
   pinning one IP? Is the validated-public-IP restriction enough?
7. **Operator precondition**: B says nothing about `FORWARD` and masquerade; a host that *does* allow
   `FORWARD` for `aekata0` (e.g. Docker `DOCKER-USER` ACCEPT) still has host-local services reachable if
   the INPUT chain is permissive; B closes that for the guest, but what other host paths exist (host
   ARP/neighbor, mDNS via the bridge device, `aekata0`'s IPv6 RA/MLD, DHCP server on the bridge)? M3
   showed the bridge device has an IPv6 link-local address that answered.
8. **Teardown races**: veth name reuse by another network after a crash while a stale table exists;
   `ctr run` failing after table install (M19 stale shim/tap state) leaving partially configured netns;
   sweep identifying "orphan" by netns existence when a sandbox is merely paused.
9. **Scaling**: O(N) chain heads per bridged frame (REASONED, not measured). Throughput and latency with
   100+ tables? Is the vmap layout needed?
10. **M8 unexplained first-run zero**: root-cause the single 0-byte download before trusting T9a as
    non-flaky.
11. **Byte accounting semantics**: L3 length, excluding Ethernet header and ARP; GSO super-packets
    counted whole (one packet can overshoot `N` by up to 64 KiB). Is "within N + one GSO packet" an
    acceptable contract for `net_bytes` (008 says budgets are enforced, I8)?
12. **Topology coupling**: B hard-depends on the bridge plugin; a deployment on `docker0`-style or a
    routed CNI fails closed by design (T11). Is E worth shipping in the same slice?
13. **Kata `tcfilter` assumption**: the design is indifferent to how Kata joins tap and veth as long as the
    guest's traffic crosses the veth host end; verify against `macvtap` internetworking_model and Kata
    config drift across versions (only 4.1.0 runtime-rs was run).

## Appendix A. Reference ruleset (as validated; generator `genb.sh`)

```
table bridge ae_k_<id> {
 counter g2w_bytes {}
 counter w2g_bytes {}
 counter drops {}
 quota budget { over <N> bytes }              # only when net_bytes > 0
 chain c_input {                               # frames from the guest toward the bridge/host
  type filter hook input priority -300; policy accept;
  iifname "<veth>" ether saddr != <mac> counter name drops drop
  iifname "<veth>" ether type arp arp operation request arp saddr ether <mac> arp saddr ip <ip> arp daddr ip <gw> accept
  iifname "<veth>" ether type arp arp operation reply   arp saddr ether <mac> arp saddr ip <ip> arp daddr ip <gw> accept
  iifname "<veth>" quota name budget counter name drops drop              # only when net_bytes > 0
  iifname "<veth>" ether type ip ip saddr <ip> ip daddr <d> tcp dport <p> counter name g2w_bytes accept   # per (d,p)
  iifname "<veth>" counter name drops drop
 }
 chain c_forward {                             # port-to-port: never allowed for this port
  type filter hook forward priority -300; policy accept;
  iifname "<veth>" counter name drops drop
  oifname "<veth>" counter name drops drop
 }
 chain c_output {                              # frames from the host stack to the guest
  type filter hook output priority -300; policy accept;
  oifname "<veth>" ether type arp arp operation reply arp daddr ip <ip> accept
  oifname "<veth>" ether type arp arp operation request arp saddr ip <gw> arp daddr ip <ip> accept
  oifname "<veth>" quota name budget counter name drops drop              # only when net_bytes > 0
  oifname "<veth>" ether type ip ip saddr <d> ip daddr <ip> tcp sport <p> counter name w2g_bytes accept  # per (d,p)
  oifname "<veth>" counter name drops drop
 }
}
```
Notes from building it: `in`, `fwd`, `out` are nft keywords (chain names `c_input` etc.); the guest answers
the gateway's ARP requests, so the *reply* rule in `c_input` is required (without it the guest's replies
were dropped; found because a 28-byte ARP frame kept hitting `drops`); `nft` counters/quota in the
`bridge` family report L3 lengths.

Not covered by this appendix but measured: candidate C's `tc1.sh` (flower + mirred + matchall drop at
pref 100) and candidate E's `ae-ptp.conflist`; they are described in M12-M15 and are not part of the
recommendation.
