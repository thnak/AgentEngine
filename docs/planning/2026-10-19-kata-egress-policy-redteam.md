# KataBackend egress policy: red-team report (issue #194)

Stage: RED-TEAM of design -> red-team -> prove -> judge. Target: `docs/planning/2026-10-19-kata-egress-policy-design.md`
(candidate B, per-sandbox nftables `bridge`-family table keyed on the veth host end). Date 2026-10-19.
Host: `thnak-server`, kernel 7.0.0-38, nftables 1.1.6, Kata 4.1.0 runtime-rs + cloud-hypervisor, CNI bridge
`aekata0` 10.88.77.0/24. No source or test file was changed. Nothing was pushed.

Tags: **MEASURED** = run, command and result given. **REASONED** = argued, not run. Priority: **blocker**
(design must change before the prover starts), **must-fix** (change required, not a redesign),
**accepted-residual** (document, ship).

## 0. Verdict and one-line findings

**VERDICT: ship-with-changes.** The enforcement point (bridge hooks keyed on the veth host end) is sound: across
about 45 hostile frame shapes and the real Kata guest there was no packet-level bypass of the exact
`(ip, tcp port)` allow set, and every probe had a positive control. But the design as written fails its own
stated goals on four axes (fail-open on table loss, O(N) cost, cross-sandbox FDB poisoning, PMTUD black hole)
and has five smaller holes. A cheaper and stronger layout exists (section 3) and was measured.

Blockers:

* **B1 (R1)** Deleting the table, or any `flush ruleset` (Debian/Ubuntu `/etc/nftables.conf` starts with one),
  silently opens the real Kata guest to the host sshd and the allowlisted path; exec-time verification detects it
  only after the fact.
* **B2 (R20)** N per-sandbox tables cost O(N x rules) per bridged frame on EVERY bridge of the host: measured
  guest throughput 19 Gbit/s at 0 tables, 4.4 at 100, 1.8 at 300, 0.94 at 600; adding one table takes 225 ms
  with 600 present. The 100+ sandbox target is not met.
* **B3 (R10)** A guest with a spoofed source MAC poisons the bridge FDB (learning runs before the netfilter
  hooks), blackholing another sandbox's traffic from the host: the design's isolation claim is false as written.
* **B4 (R13)** The output chain accepts only `tcp sport`, so ICMP frag-needed is dropped: a path with MTU below
  1500 stalls the guest after 64 KB (measured).

Must-fix: stateless return path admits unsolicited inbound TCP (R11); guest can create 40,000 host conntrack
entries in 1.2 s (R12); guest can saturate the host-wide IP fragment reassembly memory and the -300 hooks are
too late to stop it (R6); destroy ordering and sweep criterion are fail-open (R16); reload merges rules (R3);
all-A-records set is unbounded (R21).

## 1. Method and lab

Two environments, same kernel and same bridge hook code:

* **Lab** (MEASURED): own bridge `aertbr0` (10.99.77.1/24) in the host namespace, guests = netns `aert-g1`,
  `aert-g2` joined by veth `aertv1/2` (root in the netns = CAP_NET_ADMIN + CAP_NET_RAW, own MAC/IP), a "world"
  netns `aert-w` (10.99.78.2) behind veth `aertw` with listeners, host forwarding. Temporary rules carried the
  comment `ae-rt-temp` (FORWARD/INPUT accepts for my own interfaces) and were removed by `lab.sh down`
  (verified count 0). Frame crafting: raw `AF_PACKET` sockets from python3 (Appendix A), delivery observed by
  AF_PACKET sniffers in `aert-w`, `aert-g2` and on the bridge device (the bridge device taps run after the
  `input` hook, validated by the controls).
* **Real Kata** (MEASURED): one microVM `aert-k1` on the real `aekata0` with the real `ae-kata-net` CNI (cnitool
  add/del, ipMasq), image `python:3.12-alpine` (pulled by me, removed afterwards) with `--cap-add CAP_NET_ADMIN`
  and a read-only bind mount of my scripts, table loaded after `cnitool add` and before `ctr run`, as the design
  prescribes. Used to confirm the matrix, the ordering window, table loss, native port flags and tc on the real
  Kata datapath. No `ae-cap-` object was touched; `ctr c ls` was empty during my run.

Positive-control rule: every "blocked" row below was run twice, with the policy removed (probe succeeds) and with
it present (probe fails). Controls are cited per attack.

## 2. Attack list

### R1. Table loss / flush fails open (axis 1) -- BLOCKER, MEASURED

Reproduction (real Kata guest `aert-k1`, table `ae_rt_k1` = Appendix A ruleset allowing one tuple):

```
nft delete table bridge ae_rt_k1             # equivalent of any flush ruleset / nftables.service restart
ctr t exec ... python3 probe.py 10.99.78.2 443   # blocked while the table existed
ctr t exec ... python3 probe.py 10.88.77.1 22    # host sshd
```

Result: both `CONNECTED` with the table gone; `10.88.77.1:22` is `FAILED` again after reload. Nothing in the
host notices until the next `exec()`. A guest that started a background process keeps the open path between execs.
Trigger realism: `/etc/nftables.conf` on this host begins with `flush ruleset` (MEASURED `head`); the
`nftables.service` is disabled here but is enabled by default on many Debian/Ubuntu hosts and a `systemctl
restart|reload nftables` runs that file. Docker/ufw do not do this (they only touch the `ip`/`ip6` families,
MEASURED `nft list tables`), so the design's "hosts running `nft flush ruleset` are outside the supported
configuration" is optimistic: it is a single admin command.
Also: the budget counter is reset by any reinstall (quota object is recreated), REASONED from M9 semantics.
Fix: section 4 items 1-3 (shared table plus flush-proof layer plus watchdog).

### R2. Ordering window cnitool add -> nft load -> ctr run (axis 1) -- no bypass, MEASURED

Reproduction: `ip netns add; cnitool add ae-kata-net` then `tcpdump -nei <veth> -c50` for 6 s before loading the
table: 1 packet captured, from the netns's own kernel stack (no guest exists yet). Table loaded, `ctr run`
with `--with-ns network:/var/run/netns/<id>`, guest booted under policy; first probe from inside blocked. The CNI
result JSON already names the host veth (`interfaces[]` entry without `sandbox`, here `veth8187d324`) and the
guest-side MAC (`interfaces[]` entry with `sandbox`, `32:12:25:cd:83:9f`), so the design's peer-ifindex scan is
unnecessary and race-free alternatives exist. Result: the ordering in design 4.3 is correct. Note the guest MAC
equals the netns `eth0` MAC (the rule `ether saddr == mac` matched the real guest).

### R3. nft load atomicity and re-load semantics (axis 1) -- MEASURED

* Atomic: a file with table A (valid) then table B (invalid address) loaded neither (`nft -f`, then `nft list
  tables | grep -c`: 0). Design claim confirmed.
* **Merge on reload (must-fix):** `nft -f k1.nft` run again over an existing table of the same name APPENDS the
  rules (count of `ether saddr` rules 1 -> 3 after two reloads). A watchdog/reinstall that does not `delete
  table` in the same transaction accumulates stale accepts. Use `add table X` / `delete table X` / `table X {...}`
  in one file.
* nft resolves host names inside rule text (`ip saddr 999.1.1.1` produced "Could not resolve hostname"), so any
  non-literal reaching the generator becomes a DNS lookup and a parse surface: T19 is mandatory, and the
  generator must accept only typed `uint32` addresses.
* Hyphenated ids (`ae_k_ae-kata-1a2b3c`) are valid table names (MEASURED), so the id charset needs no mapping.

### R4. br_netfilter and bridge conntrack (axis 1) -- PARTLY MEASURED

`br_netfilter` was NOT tested: it is a host-global module and loading it would change firewalling for every
tenant of the host (and the capacity test). REASONED: with it loaded, bridged IP also traverses iptables FORWARD
after our -300 chains; additive only. MEASURED instead: using `ct state` in a bridge chain loads
`nf_conntrack_bridge` (lsmod refcount 4 -> 5), which defragments IPv4 at bridge PRE_ROUTING (priority -400),
BEFORE the design's -300 `input`/`forward` chains. Effect measured in R6.

### R5. Is a shared default-drop base chain needed? (axis 1) -- YES, MEASURED

Layout in section 3. With a shared table whose dispatch map has no entry for a port, that port is dropped (`meta
ibrname` default drop): guest `aert-g2` with no per-sandbox rules could not connect to anything (control: with the
table deleted it could). Removing only a sandbox's map elements while the guest runs also fails closed (both
directions timed out, MEASURED), so a destroy/cleanup bug can no longer open an unrelated or still-running guest.
It does NOT help against deleting the whole table (R1).

### R6. IP fragments (axis 2) -- accepted-residual for bypass, MUST-FIX for DoS, MEASURED

Reproduction (lab, `cases1.py` c03-c05 in Appendix A; allowed tuple `10.99.78.2:443`):

| Frame | Without ct rule | With `ct state` rule in table |
|---|---|---|
| first fragment (off 0, MF) tcp dport 443 | reaches host (`aertbr0` sniff), not forwarded (host defrag holds it) | never reaches host (reassembled first, incomplete) |
| non-first fragment (off 1) with bytes[2:4]=443 | **reaches host**: nft's `tcp dport` match does NOT exclude non-first fragments | never reaches host |
| non-first fragment with bytes[2:4]=22 | dropped | dropped |

No path to another destination or port exists: any reassembled datagram needs an offset-0 fragment, which must
pass `tcp dport` itself. Control (no table): all fragments reached the host. Result: no bypass; the semantic gap
(a non-first fragment matched as TCP) is harmless but should not be relied on.
**DoS (must-fix):** `fragflood.py` sends 30,000 first-fragments with distinct ids to the allowed tuple;
`/proc/net/sockstat` `FRAG: inuse 1676 memory 4196704` = `ipfrag_high_thresh` (4 MiB) for 30 s, host-wide
(drained after expiry). The same drop rule placed at `input` priority -300 does NOT help (memory still 4196704)
because bridge conntrack defragments earlier (-400). A rule in a **`prerouting` chain at priority -450**
(`iifname veth ip frag-off & 0x3fff != 0 drop`) keeps memory at 0 (MEASURED). TCP does not need guest-side
fragmentation (DF), so dropping all fragments from the port is safe.

### R7. IP options, ICMP, UDP/53, IPIP, GRE to an allowlisted address (axis 2) -- no bypass, MEASURED

Lab and real Kata, allowed tuple `D:443` (lab) / `D:9000` (real): options (IHL 7) to `:443` passes (it is a
legitimate TCP segment), options to `:80` dropped; UDP dst 443 and UDP dst 53 to D dropped; ICMP echo to D dropped;
IP protocol 4 (IPIP) and 47 (GRE) to D dropped. Controls: with the table removed every one of these reached the
world netns. UDP/53 DNS exfil is therefore closed; a TCP/53 entry is just another allowed TCP tuple (inherent).

### R8. VLAN, QinQ, non-IP, link-local, IPv6, mDNS/DHCP-class traffic (axis 2) -- no bypass, MEASURED

Frames sent: 802.1Q vid 0/1/5 and 802.1ad+802.1Q (IP tcp 443 and 80 inside), also with bridge `vlan_filtering 1`
on my lab bridge; ethertype 0x88b5 unicast/broadcast; BPDU (01:80:c2:00:00:00); LLDP; IPv4 multicast UDP/5353;
IPv6 UDP/5353 and TCP/22 to the bridge's fe80 address; broadcast-MAC frame carrying an allowed IP tuple.
Result with table: none reached the world, the guest peer or the bridge device (exception: the broadcast-MAC
frame with the allowed tuple reached the host input, where the IP layer discards a non-host-addressed broadcast;
it was not forwarded). Controls: without the table VLAN frames (even vid 0 to port 80), mDNS, IPv6 to host
reached the world or the bridge device, with and without vlan_filtering. DHCP is covered by the same default-drop
(`ip saddr 0.0.0.0`/broadcast fail the `ip saddr == guest ip` and tuple rules), REASONED, c21 is the stand-in.
Same matrix on the real Kata guest with `CAP_NET_ADMIN`: only the legitimate ARP request got through
(`aekata0` sniff `AERT-c23_arp_ok_req_bcast: 3`); control (table deleted): 24 distinct markers reached the bridge.

### R9. ARP spoofing and poisoning (axis 2) -- no bypass, MEASURED (design item M20 was untested)

Crafted frames from the guest: gratuitous ARP reply claiming the gateway IP, gratuitous ARP claiming another
guest's IP, ARP request for a victim IP, unicast ARP to another guest's MAC, ARP with forged `arp sha` inside a
correct Ethernet source, all with sender MAC = guest MAC. With the table: none delivered (to the bridge device
or to the victim guest). Control (no table): all reached the bridge/victim and poisoned the host neighbour table
(`10.99.77.12 -> 02:..:11`, `10.99.77.11 -> 02:..:77` observed with `ip neigh`), reproduced on the real Kata
guest. Legit request/reply to the gateway passes. Residual: ARP frames to the gateway may carry arbitrary
trailing padding (the marker was visible on the bridge device): a covert channel to the host kernel only, no
external reach (accepted).

### R10. FDB poisoning by spoofed source MAC (axis 2/6) -- BLOCKER, MEASURED

Reproduction (lab): g2 pings the gateway so FDB learns `02:..:12 -> aertv2`; host `ping 10.99.77.12` works (0%
loss). g1, whose port carries the design's table, sends 3 frames with Ethernet source `02:00:00:00:00:12`
(ethertype 0x88b5, so `ether saddr != mac` drops them). `bridge fdb show` now maps `02:..:12 -> aertv1`; host
`ping 10.99.77.12` = 100% loss. Recovers only when g2 transmits again. Cause: the bridge learns the source
(`br_fdb_update`) before the NF_BR_LOCAL_IN / FORWARD hooks run, so the rule that drops the frame is too late.
A hostile sandbox can keep any other sandbox black-holed from the host/gateway. The design's "port-keyed spoof
resistance" and "stops other sandboxes talking to this one" cover IP traffic only, not this.
Fix, MEASURED: `bridge link set dev <veth> learning off locked on isolated on` plus
`bridge fdb add <guest mac> dev <veth> master static` (3 commands, survive nft loss): after the same spoof the FDB
entry stayed on `aertv2`, host ping 2/2; the real Kata guest kept working with the static entry (probe
CONNECTED), and when the guest (CAP_NET_ADMIN) changed its own MAC the connection FAILED, restoring the MAC
CONNECTED. Control of "locked on needs the entry": removing the static entry made the guest's traffic fail.
`isolated on` on both lab ports blocked g1<->g2 TCP (timeout), the gateway stayed reachable. Note `locked on`
does not fire for MACs already learned dynamically; `learning off` must be set first (observed: a spoof MAC
learned during a prior control run passed until its entry was deleted).

### R11. Stateless return path admits unsolicited inbound (axis 2) -- MUST-FIX, MEASURED

Reproduction (lab): world netns binds source port 6000 and connects to the guest's listener
`10.99.77.11:7777`; allowed tuple `10.99.78.2:6000`. Without table: connected. With the design's table:
`INBOUND CONNECTED b'HELLO7777'` (the output rule accepts `ip saddr D tcp sport P` to the guest, any dport, any
flags; the guest's SYN-ACK leaves because its `tcp dport 6000` matches the allowed tuple). With
`ct state established,related` added to the output accept: `INBOUND FAILED timed out`, and guest-initiated
outbound still works (CONNECTED, including the RST path). Requires the attacker to be able to route to
10.88.77.0/24 through the host (LAN host plus a permissive `FORWARD`, e.g. DOCKER-USER ACCEPT), so this is a
conditional inbound channel into a sandbox, not an egress bypass; the fix is one match and was measured not to
break the return path. `nf_conntrack` is already loaded by Docker/ufw/CNI here.

### R12. Conntrack exhaustion from one guest (axis 6) -- MUST-FIX, MEASURED (extrapolated)

Reproduction: 40,000 raw SYNs with distinct source ports and valid TCP checksum to the allowed tuple (`flood.py`,
1.18 s). `/proc/sys/net/netfilter/nf_conntrack_count` 20 -> 40016 (max 262,144, so the table fills in about 8 s;
this extrapolation is REASONED). Entries are shared with the host and every tenant. Mitigation, MEASURED: in
`c_input` before the accept `tcp flags & (syn|ack) == syn limit rate over 100/second burst 50 packets drop`:
same flood left count at 115 and a legitimate connect afterwards succeeded. Also MEASURED
`nf_conntrack_tcp_timeout_established = 432000` (5 days): an established flow cut by the quota or by a VM kill
lingers in the host table; accepted, but sized with the rate limit.

### R13. PMTUD black hole (axis 2/4) -- BLOCKER (functional), MEASURED

Reproduction (lab): host-side link MTU to the world set to 1400 (`ip link set aertw mtu 1400`), guest uploads 600
kB to the allowed tuple. With the design's output chain: `SEND stopped TimeoutError after 65536 bytes`. ICMP
frag-needed generated by the host is dropped by the output chain's default drop. After adding
`ip protocol icmp ct state related accept` to the output chain: `SENT all 600000` and the guest route cache
showed `mtu 1400`. Any path with MTU < 1500 (PPPoE, VPN, tunnels, some clouds) stalls guests. Fix is the one
rule; only ICMP errors for tracked flows are admitted.

### R14. net_bytes accounting (axis 4) -- accepted-residual with contract changes, MEASURED

* Budget enforced both directions, shared: upload to an allowed sink with `N=200000`: `quota ... used 200000
  bytes`, `g2w_bytes` counted 11 packets / 197,188 B (GSO super-packets of up to 64 KB, avg 18 KB; veth has
  TSO/GSO on), the guest socket then stalled (`SEND stopped TimeoutError after 589824 bytes`, the kernel buffer),
  and a fresh connect to the same tuple timed out. Delivered <= N in L3-skb bytes; the packet that crosses the
  limit is dropped whole (so under-delivery up to one GSO packet, never over-delivery) -- the design's "N + one
  GSO packet" contract is too pessimistic, but wire bytes exceed accounted bytes by the replicated headers of
  segmentation (about 2-6%, REASONED from MSS 1448 and 66 B of headers).
* Not counted: ARP frames (accepted before the quota rule), Ethernet headers. Counted although useless: junk
  frames to disallowed destinations (they reach the quota rule first) so a guest can burn its own budget.
* Failure is silent: no RST/ICMP, the tool sees a stall until its own timeout, FIN/RST from the guest are
  dropped too, leaving the destination half-open (R12 timeouts). Optional: `reject with tcp reset` after
  exhaustion gives an error instead of a hang (not tested, REASONED).
* Budget reset on reinstall (R1).
* M9 re-measured (shared quota, cut at about N, rest dropped): consistent with the design.

### R15. M8 first-run zero-byte download (axis 9) -- NOT REPRODUCED

40 consecutive 1 MiB downloads through the table (20 stateless, 20 with `ct`), then 30 with the host and guest
neighbour caches flushed before each run: 70 of 70 transferred exactly 1,048,576 B. No root cause found; the
original flake remains unexplained (candidate: first-ARP race on the real Kata guest). The prover should keep T9a
tolerant of one retry only if the cause is found, otherwise treat it as open.

### R16. Teardown, sweep and crash races (axis 5) -- MUST-FIX, MEASURED + REASONED

* **Sweep keyed on `/var/run/netns/<id>` fails open (MEASURED):** `ip netns add X; veth into the bridge; hold a
  process inside; ip netns del X` removes the path while the process, the netns and the bridge port stay
  alive. The Kata shim/VM holds the netns the same way, so any `ip netns del` (cleanup script, tmpfiles, a second
  backend instance, another tool cleaning `ae-kata-*`) makes the sweep delete the table of a LIVE guest. Same
  in a backend that runs in a container without `/var/run/netns` (REASONED). Sweep must key on the bridge port:
  delete a table only when its veth no longer exists.
* **Destroy order (REASONED from `kata_backend.cpp` lines 940-996):** failures of `ctr task kill` and `cnitool del`
  only print to stderr; the design deletes the table right after the kill. If the kill silently failed the guest
  runs with the table gone until `cnitool del` removes the port (and if that fails too, forever). The policy must
  outlive the port: delete it only after the veth is gone (or the shim is confirmed dead).
* veth reuse: CNI names the host veth `veth` + 8 hex (observed `veth8187d324`), so a stale table matching a
  future veth name needs a 2^-32 coincidence and would only add drops (fail closed). Not an attack.
* Failed `ctr run` (M19): not re-run; cleanup_partial covers table, netns, CNI. The CNI-reported veth lets
  cleanup avoid guessing.
* Two processes on one host: ids are 64-bit random (`fresh_id`), no collision; a sweep per process is safe only
  with the port-existence criterion. `nft ... flags owner` must NOT be used for these tables (REASONED: an owner
  table is removed when the creating process exits, which is fail-open on a host-process crash while the VM
  keeps running).
* `ctr run` with no network namespace (the `deny_all` shape) makes the shim create a `cnitest-*` namespace file
  that is never deleted (count 43 -> 44 for my one run, removed by me, identified by mtime). Pre-existing, noted.

### R17. Host-side collateral: global hooks, Docker/ufw (axis 6) -- MUST-FIX via R20, MEASURED

Bridge-family hooks are global across ALL bridges: each frame on docker0 or any other bridge also walks every
`ae_k_*` chain head. R20 numbers therefore apply to unrelated tenants as well (the fake tables in the
measurement keyed non-existent ifnames, so the cost was pure walk). Docker/ufw/iptables objects are in the
`ip`/`ip6` families (MEASURED `nft list tables`) and untouched by the design; this host has `br_netfilter`
unloaded and `nftables.service` disabled; hosts with those differently configured: R1, R4. The proposed shared
table restricts its drop to `meta ibrname "aekata0"`, so other bridges only pay one hash lookup (MEASURED flat).

### R18. Unrestricted/empty/deny_all shapes (axis 7) -- no bypass, MEASURED + code read

* `deny_all` (the OCI spec has a `network` namespace without a path): guest `/proc/net/dev` only `lo`, bridge
  gained 0 ports (MEASURED on a real microVM with the same spec shape).
* Empty allowlist with `deny_all=false`: rejected by `authorize_spec()` when any grant exists, by the
  backend's own `net_capability_required` when none does (code read, `kata_backend.cpp` lines 157-186,
  `sandbox.hpp` 256-290). Every tuple traces to `spec.net.allowlist`, which must be covered by an exact
  (case-insensitive) string in a `cap::SandboxNetOut` grant. I2 holds for the design.
* Spec ambiguity (accepted-residual): `deny_all=true` with a non-empty allowlist enables networking (the
  backend computes `wants_network = !deny_all || !allowlist.empty()`), i.e. "deny_all" is not authoritative. Should
  reject or document.

### R19. I3 and DNS (axis 3) -- must-fix (bound) / accepted-residual

How a model can influence the allowlist today: no engine code path derives `spec.net.allowlist` or a
`SandboxNetOut` grant from tool arguments (`grep` over `include/` and `src/` outside the Kata backend finds
only `sandbox.hpp` coverage code and the backends that refuse allowlists). The allowlist strings are
host-authored; the model can only (a) choose when a sandbox is created and (b) influence DNS answers for a
granted NAME if it has another tool that can edit that zone (dynamic DNS, a domain it controls and the host
granted). Today the name resolves to one validated public IP, so the influence is one address; under the design
it becomes every A record (up to the DNS answer size, TCP DNS allows thousands). That multiplies rules and the
reachable IP set at the name owner's discretion. Resolve-time vs connect-time drift cannot widen anything (the
rule holds IP literals, no later resolution; DNS rebinding after create has no effect, which is good) but loses
connectivity when a CDN rotates (availability, accepted). Required: cap the record count (suggest 8, fail
create or truncate deterministically), keep `resolve_and_validate` per record, state in the ADR that a grant on a
NAME is a grant on its create-time public record set. Host-local rejection (design 4.4) is correct and should
also reject the host's NAT-hairpin WAN address where known (accepted-residual: unknowable).

### R20. Scaling to 100+ sandboxes (axis 8) -- BLOCKER, MEASURED

Reproduction: N fake per-sandbox tables (design Appendix A text, distinct ifnames and ips, 2 tuples each) plus
the real guest table; guest netns uploads 1.5 GB to a sink in the world netns, 3 runs per N.

| Layout | N=0 | N=100 | N=300 | N=600 |
|---|---|---|---|---|
| per-sandbox tables (design) Gbit/s | 17.9-19.2 | 4.4 | 1.8 | 0.94 |
| shared table + `iifname vmap` dispatch Gbit/s | 18.6-19.2 | 18.2-18.9 | 18.2-19.2 | 19.0-19.4 |

Control-plane at N=600 present: per-sandbox design add one table 225 ms, delete one 231 ms, `nft list tables`
220 ms; shared design add one sandbox (chains plus 3 map elements) 24 ms, delete 29 ms, bulk-add 600 in 124 ms.
(Host load average was about 0 during measurement.) The shared layout also gives R5.

### R21. Cheaper or stronger designs the author missed (axis 9) -- MEASURED

1. **Shared table with verdict-map dispatch** (R5, R20): same rules, one table. Reference text Appendix B.
2. **Native bridge port flags** (R10): `learning off`, `locked on`, `isolated on` and a static FDB entry per
   port. Three iproute2 commands, no netfilter, survive any nft flush, close MAC spoofing, FDB poisoning and
   guest-to-guest at L2 by construction.
3. **Host-end tc clsact** (flush-proof allowlist): on the veth HOST end, which Kata never touches (only the netns
   `eth0` and `tap0_kata`), install default-deny FIRST then allow filters; partial installation is only more
   restrictive (fail-closed ordering by construction). Measured parity with the nft matrix, lab and real Kata:
   with no nft table, only the legitimate tuple and ARP passed, VLAN/QinQ/frag/guest-guest/forged ARP all
   dropped, TCP to the allowed tuple worked, host sshd and unlisted ports blocked. Required flower pins that a
   first attempt missed: `dst_mac <bridge mac>` (otherwise allowed-tuple frames with a victim's MAC and
   broadcast ARP reach other guests, observed) and `arp_sha`. Egress filters on every port also stop floods to
   other guests. It cannot do the byte quota, `ct`, rate limit or ICMP-related, so it is a second layer, not a
   replacement (the design's rejection of candidate C in-netns on race/quota grounds does not apply to the host
   end). Ruleset in Appendix B.

### R22. Attacks that FAILED (covered, no bypass) -- MEASURED unless noted

TCP to unlisted port of the allowed IP; unlisted IP; wrong source IP; wrong source MAC; UDP/ICMP/IPIP/GRE to the
allowed IP; 802.1Q/802.1ad with every vid; non-IP ethertypes; BPDU/LLDP; IPv4 multicast; IPv6 to the bridge;
forged ARP in six variants; guest-to-guest TCP both ways; unicast frames with another guest's MAC; broadcast
frames reaching a peer (peer sniffer empty with table, populated without); fragments to other ports; IP options
to other ports; guest host-local reach (gateway, host sshd): all blocked on the real Kata guest as well; atomic
partial-load; ordering window.

### R23. Not testable here

* `br_netfilter` loaded (global module, would disturb other tenants and the capacity test): REASONED only.
* Guest-to-guest between two REAL microVMs (lab veth guests were used; real single microVM for everything else).
* A real internet path (used the lab world netns behind CNI masquerade; ICMP/PMTUD and conntrack results hold).
* Exhaustion of the full 262,144 conntrack table (stopped at 40,016 to avoid harming the host).
* Kernels other than 7.0.0-38; `macvtap` internetworking model; Kata `tcfilter` drift (design Q13).
* `flags owner` crash behaviour (REASONED), `reject with tcp reset` (REASONED), multi-A DNS (no multi-A name
  exercised; R19 is REASONED), the M5 kernel mechanism, the root cause of the M8 flake.

## 3. Recommended layout (measured)

```
per backend (once):   table bridge ae_k { set ports; map in_d; map out_d;
                        chain prerouting @-450: iifname @ports frag drop
                        chain input @-300:  iifname vmap @in_d ; meta ibrname "<bridge>" drop
                        chain forward @-300: iifname @ports drop ; oifname @ports drop ; meta ibrname "<bridge>" drop
                        chain output @-300: oifname vmap @out_d ; meta obrname "<bridge>" drop }
per sandbox (one transaction, element adds last): chains in_<id>/out_<id>, per-sandbox quota,
                        ports/in_d/out_d elements
per sandbox (before ctr run): bridge link set learning off locked on isolated on; static FDB entry;
                        optional tc clsact host-end allowlist (default-deny first)
```

## 4. Design changes required (ordered)

1. **Replace per-sandbox tables with one shared `ae_k` table**, per-sandbox chains and a quota object added and
   removed by element/chain transactions, dispatch by `iifname vmap`, default drop for any port of the CNI
   bridge (`meta ibrname`/`obrname`). Verify the table once per exec and by a watchdog. Closes B2, R5, R17. Reload
   = `add table; delete table; table {...}` in one file (R3).
2. **Native port hardening before `ctr run`** (mandatory): `learning off`, static FDB entry for the CNI guest
   MAC, `locked on`, `isolated on`. Closes B3 and part of B1 at L2. Take the veth and MAC from the CNI result,
   not an ifindex scan.
3. **Flush-proof egress layer or a watchdog that fails closed.** Required minimum: a host-process watchdog
   (every few seconds, plus on every exec) that re-verifies the table, reinstalls it (delete+add, counters
   restored from host-side accounting) and kills the guest if reinstall fails; ADR states honestly that a
   flush opens egress until the next tick. Recommended: add the tc host-end allowlist (R21.3), default-deny
   first, so a flush cannot open egress at all and nft then only provides quota/ct/rate. Decide in the
   prove/judge stage; either way document the residual.
4. **Early `prerouting` chain at priority -450** dropping IP fragments (and optionally wrong-MAC frames) from the
   port, before bridge conntrack defragments (R6).
5. **Output chain**: `ct state established,related` on the tcp accept (R11) and `ip protocol icmp ct state
   related accept` (R13).
6. **Rate limit** new connections from the port (`tcp flags & (syn|ack) == syn limit rate over R/s drop`) and
   document the host conntrack sizing and 5-day established timeout (R12). Optionally limit ARP.
7. **Teardown**: delete policy only after the veth/port is confirmed gone; sweep keyed on port existence (store
   id and veth in a table/chain comment), never on `/var/run/netns/<id>`; do not use `flags owner` (R16).
8. **DNS**: cap the A-record count (suggest 8), keep per-record validation, state in the ADR that a name grant
   is a grant on the create-time public record set; reject `deny_all=true` plus non-empty allowlist or define it
   (R18, R19).
9. **`net_bytes` contract text**: delivered L3 skb bytes <= N (including both directions, excluding Ethernet and
   ARP, not exact wire bytes), crossing packet dropped whole, silent stall; reset on reinstall must be
   prevented (watchdog restores counters) (R14).
10. **Tests to add to section 6** (each with its positive control): T-FDB (R10), T-PMTU (R13), T-INB (R11), T-FRAG-MEM
    (R6), T-CT-FLOOD (R12), T-FLUSH (delete table, guest stays blocked with layer 3 or is killed by the
    watchdog; control: today's design opens), T-SCALE (100 tables/sandboxes keep >= 90% of baseline throughput, R20),
    T-SWEEP (`ip netns del` of a live sandbox does not drop its policy, R16), T-RELOAD (no duplicate rules, R3).

## 5. Accepted residuals (with justification)

* Allowed destination is a tunnel or open relay, TCP covert channel to it: inherent to any allowlist.
* ARP padding as a covert channel to the host kernel only; unrated ARP to the gateway (host CPU only): no
  external reach, keep a rate limit optional.
* `net_bytes` is L3-skb accounting, not wire bytes (2-6% under-count), ARP excluded, junk burns own budget:
  I8 is honoured as "a hard cap on delivered payload", say so.
* Name-grant authority is the create-time public record set (R19): same trust class as ADR-093's single IP,
  bounded by the cap and by `resolve_and_validate`.
* NAT-hairpin WAN address of the host cannot be known to the local-address check: operator topology.
* `br_netfilter` untested (R4): additive only by reasoning; note in the ADR and test when a host with it is
  available.
* A flush between watchdog ticks (if layer R21.3 is not adopted): bounded window, documented.
* The established-conntrack 5 day linger after quota/kill: host sysctl, bounded by the SYN rate limit.
* Silent stall instead of a clean error at quota exhaustion unless `reject with tcp reset` is adopted.
* The pre-existing leaked `cnitest-*` namespace per default-shape `ctr run` and the stale shim socket (Finding 3).

## Appendix A. Reproduction toolkit (lab, all objects prefixed `aert`)

```
# lab: bridge aertbr0 10.99.77.1/24; netns aert-g1/g2 (veth aertv1/2 <-> eth0, MAC 02:00:00:00:00:1x,
#      ip 10.99.77.1x, default via .1); netns aert-w (veth aertw 10.99.78.1/24 <-> eth0 10.99.78.2, route
#      10.99.77.0/24 via 10.99.78.1); listeners in aert-w on 443/80/53/5000/9000; temp rules, all with
#      `-m comment --comment ae-rt-temp`: FORWARD -i aertbr0 -o aertw ACCEPT, FORWARD -i aertw -o aertbr0 ACCEPT,
#      INPUT -i aertbr0 ACCEPT, INPUT -i aertw ACCEPT ; teardown loops `iptables -D` on grep ae-rt-temp.
# crafting (python3, root, run inside the guest netns, AF_PACKET SOCK_RAW bound to eth0):
#   eth(dst,src,ethertype,payload); ip4(src,dst,proto,payload,id,mf,off,opts) with header checksum;
#   tcp(sport,dport,data) + tcpck() pseudo-header checksum; arp(op,sha,sip,tha,tip)
#   every payload ends with a marker b"AERT-<case>"; sniffers (AF_PACKET proto ETH_P_ALL) in aert-w, aert-g2 and
#   on aertbr0 count markers for 6 s. Cases c01..c28 = the rows in R6-R9 (dst MAC = bridge MAC unless stated).
# design ruleset = design Appendix A (generated with the shown veth/mac/ip/gw); `ct state established,related`
#   added to the output tcp accept for R11+ runs.
# flood:   for i in 0..39999: SYN sport 1024+i%60000 to allowed tuple          -> nf_conntrack_count 40016
# fragflood: 30000 x (ip id=i, MF=1, off=0, tcp dport 443, 1400 B)             -> FRAG memory 4196704
# poison:  3 x eth(bridge_mac, victim_mac, 0x88b5, b"AERT-fdb") from the attacker port; `bridge fdb show`
# native:  bridge link set dev V learning off locked on isolated on; bridge fdb replace <mac> dev V master static
```

## Appendix B. Measured alternative rulesets

Shared table (the one measured at N=600; per-sandbox part generated):

```
table bridge ae_k {
 set ports { type ifname; }
 map in_d  { type ifname : verdict; }
 map out_d { type ifname : verdict; }
 chain c_input   { type filter hook input   priority -300; policy accept;
                   iifname vmap @in_d ; meta ibrname "aekata0" drop }
 chain c_forward { type filter hook forward priority -300; policy accept;
                   iifname @ports drop ; oifname @ports drop ; meta ibrname "aekata0" drop }
 chain c_output  { type filter hook output  priority -300; policy accept;
                   oifname vmap @out_d ; meta obrname "aekata0" drop }
}
# per sandbox <tag> (single nft -f transaction):
add chain bridge ae_k in_<tag>
add chain bridge ae_k out_<tag>
add rule  bridge ae_k in_<tag> ether saddr != <mac> drop
add rule  bridge ae_k in_<tag> arp operation request arp saddr ether <mac> arp saddr ip <ip> arp daddr ip <gw> accept
add rule  bridge ae_k in_<tag> arp operation reply   arp saddr ether <mac> arp saddr ip <ip> arp daddr ip <gw> accept
add rule  bridge ae_k in_<tag> ip saddr <ip> ip daddr <d> tcp dport <p> accept          # per (d,p), after limit/quota
add rule  bridge ae_k in_<tag> drop
add rule  bridge ae_k out_<tag> arp operation reply arp daddr ip <ip> accept
add rule  bridge ae_k out_<tag> arp operation request arp saddr ip <gw> arp daddr ip <ip> accept
add rule  bridge ae_k out_<tag> ip protocol icmp ct state related accept
add rule  bridge ae_k out_<tag> ip saddr <d> ip daddr <ip> tcp sport <p> ct state established,related accept
add rule  bridge ae_k out_<tag> drop
add quota bridge ae_k q_<tag> { over <N> bytes }        # + `quota name q_<tag> drop` inserted first in in_/out_
add element bridge ae_k ports { "<veth>" }
add element bridge ae_k in_d  { "<veth>" : jump in_<tag> }
add element bridge ae_k out_d { "<veth>" : jump out_<tag> }
```

Host-end tc layer (veth host end `V`, guest `M`/`I`, bridge `BRM`, gateway `GW`, allowed `D:P`), default-deny first:

```
tc qdisc add dev V clsact
tc filter add dev V ingress pref 100 matchall action drop
tc filter add dev V egress  pref 100 matchall action drop
tc filter add dev V ingress pref 10 protocol arp flower src_mac M dst_mac ff:ff:ff:ff:ff:ff arp_op request arp_sha M arp_sip I arp_tip GW action ok
tc filter add dev V ingress pref 11 protocol arp flower src_mac M dst_mac BRM arp_op reply   arp_sha M arp_sip I arp_tip GW action ok
tc filter add dev V ingress pref 12 protocol arp flower src_mac M dst_mac BRM arp_op request arp_sha M arp_sip I arp_tip GW action ok
tc filter add dev V ingress pref 13 protocol ip  flower src_mac M dst_mac BRM src_ip I dst_ip D ip_proto tcp dst_port P action ok
tc filter add dev V egress  pref 10 protocol arp flower src_mac BRM arp_op reply arp_tip I action ok
tc filter add dev V egress  pref 11 protocol arp flower src_mac BRM arp_op request arp_sip GW arp_tip I action ok
tc filter add dev V egress  pref 13 protocol ip  flower src_mac BRM src_ip D dst_ip I ip_proto tcp src_port P action ok
```

Server hygiene at the end: 0 `ae-rt-temp` rules in iptables, 0 `ae_rt_*`/`ae-rt` objects in nft, no `aert*` links,
netns, processes or files, `ctr c ls` empty, my image pulled (`python:3.12-alpine`) removed, my stray
`cnitest-*` namespace file removed. No apt installs, no pip installs (python3 stdlib only). Host sysctl, Docker,
ufw and persistent firewall untouched; the transient FRAG and conntrack load drained by expiry (0 and 15 after).
