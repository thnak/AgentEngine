# ADR-239: Kata egress policy -- the enforcement point is the host-namespace bridge port

Status: **Proposed** (2026-10-19) -- designed, red-teamed and proven; awaiting the Judge. Not Judged.
Issue: #194. Supersedes in part: ADR-093 (the `ip netns exec <id> nft ... ae_netpolicy` allowlist) and ADR-095 (the
`net_bytes` quota object in that same table). Both carry a banner pointing here.
Records: `docs/planning/2026-10-19-kata-egress-policy-design.md`, `...-redteam.md` (revised design in its section 6).
Invariants: I2 (no ambient authority), I3 (model output is data), I4, I8 (budgets enforced).

## 1. Problem

ADR-093/095 enforced `NetPolicy::allowlist` and `ResourceLimits::net_bytes` with nftables rules inside the sandbox
network namespace. Live finding 2 (2026-10-19): Kata joins the guest tap and the veth with `tc mirred` redirects, so
the guest's frames never traverse that namespace's netfilter hooks. The rules were accepted, listed and inert: a guest
could reach the host's sshd and the LAN. Every earlier "enforcement" test used a namespace process, not a microVM.

## 2. Decision

Enforce on the **host end of the veth, a port of the CNI bridge in the host namespace**, in three independent layers:

1. **Native port flags and static FDB**: `learning off`, `locked on`, `isolated on`, one static FDB entry. Closes
   source-MAC spoofing, FDB poisoning and guest-to-guest; survives any nft flush.
2. **`tc clsact` flower allowlist** on the veth host end, default-deny first (a partial batch is only more
   restrictive): exact (guest MAC/IP, destination, tcp port) tuples, exact ARP to the gateway, ICMP type 3 only, no
   fragments. Survives any nft flush.
3. **One shared nft `bridge` table `ae_k`** with `iifname` vmap dispatch to per-sandbox chains and a default DROP for
   any port of the CNI bridge without a map entry. Carries conntrack state, SYN-rate limit, the named byte quota and the
   early (priority -450) fragment drop.

Ordering is fail-closed. Create: sweep -> flags -> FDB -> tc -> base ensure -> sandbox transaction -> read-back and
snapshot. Destroy: the port is removed FIRST; policy is removed only after the port is gone; if the port persists the
policy is kept (`kata_backend.egress_policy_kept`). Sweep keys on port existence, never on a netns path. Exec-time
verification compares flags, FDB, base chains and byte-equal snapshots of the sandbox's nft chains and tc listings
(one retry after 200 ms); a persistent mismatch kills the guest and returns `kata_backend.egress_policy_missing`.

I2/I3: every tuple derives from an allowlist entry already required to be covered by a `cap::SandboxNetOut` grant;
hostnames are resolved once by backend code (cap 8 A-records per entry, 64 total, host-local addresses rejected); the
generators accept only typed values (uint32, uint16, uint64) or charset-validated MAC/ifname/tag strings. The backend
only adds drops: it never edits iptables, ufw, Docker or any sysctl.

`net_bytes` contract (changed from ADR-095): one named quota per sandbox shared by both directions; delivered L3 bytes
never exceed N; the crossing packet is dropped whole; the failure is a silent stall; the quota dies with the table.

## 3. Why not the alternatives

- Per-sandbox tables: measured guest throughput 7.99 Gbit/s at 100 sandboxes against 24.68 unpoliced (section 5, S1).
- Rules in the sandbox netns: inert (section 1).
- nft only: a table loss opens the guest (R1, blocker). Layers 1-2 remain when nft is gone.
- Trust the guest's MAC/ARP: FDB poisoning of a neighbour measured in the control (R10).

## 4. Evidence (claim -> test -> positive control -> result)

Tests run on `thnak-server` with real Kata microVMs (python:3.12-alpine guest, CAP_NET_RAW for raw frames, lab world on
TEST-NET-2). "Control" is a hand-wired sandbox with no policy, which must show the attack succeeding.

| # | Claim | Test | Positive control | Result |
|---|---|---|---|---|
| E1 | Allowlisted (ip,port) reachable; same IP other port and unlisted IP blocked | egress_linux | control reaches all three | held |
| E2 | Host sshd (gateway + LAN IP), IPv6 link-local, guest-to-guest blocked | egress_linux | control reads banners/peer | held |
| E3 | Hostile frames (spoofed MAC/IP, VLAN, QinQ, frags, GRE, IPIP, UDP, ICMP, IPv6, raw ethertype, forged ARP) not delivered | raw-frame batch sniffed on bridge and world side | control delivers 13 classes | held (all 0 with policy) |
| E4 | Spoofed source MAC does not poison a neighbour's FDB; forged ARP does not poison host neighbours | polling FDB/neigh probe | control shows victim behind two ports | held |
| E5 | Conntrack flood bounded | 20000 SYNs | control forwards 20000 (+19997 ct) | held: 102 forwarded (+99) |
| E6 | Fragment-memory flood bounded | frag flood | control peak 4,196,016 B | held: 0 B |
| E7 | Unsolicited inbound dropped | inbound connect | control connects | held |
| E8 | PMTUD works (frag-needed reaches guest) | 600 kB send | rule deleted -> stalls at 65536 B | held |
| E9 | `net_bytes` counts inbound, outbound and one shared budget | bulk transfer both ways | no-quota control moves 1,000,000 | held: <= N=200000 in each case |
| E10 | Table loss (`delete table`, flush) still contains the guest | probes after tc+nft layers selectively removed | with tc also removed, host sshd reachable | held |
| E11 | Exec-time verification detects table loss and tamper, kills guest, refuses | tamper into in_ and out_ chains | untampered exec runs; tamper opens host sshd | held |
| E12 | tc removed: nft + flags alone contain | remove tc | both removed -> host reachable | held |
| E13 | Teardown leaves no port, netns, container, table, chain, quota, file | leftover scan | n/a | held |
| E14 | Parallel create/probe/destroy of two sandboxes on one backend | lifecycle_linux | each reaches only its own tuple | held |
| E15 | Sweep keys on the port, not `/var/run/netns` | live entry survives `ip netns del`; dead entry swept | n/a | held |
| E16 | Install fails closed at every step (injected faults) | fake nft/tc/bridge on PATH | n/a | held; no further effect after failure |
| E17 | Host firewall unchanged by a full run | iptables-save diff (counters stripped) | n/a | held |
| E18 | A-record cap, host-local rejection, hostile tag/name/MAC refused by every generator | render unit test | n/a | held |
| E19 | Real internet path through the backend (slice9_10 cases 6/9/9b) | requires temporary FORWARD accepts for `aekata0`<->uplink, removed in the same command | case 9b control | held (ALL PASS) |

Suites: egress_linux 73/73, lifecycle_linux 119/119, render unit test all pass, scale_linux all pass, existing
test_kata_backend_linux, slice2, abuse_corpus, slice9_10 all pass; gcc and clang builds of the Kata targets clean.

### 5. Scaling (S1), lab netns guests behind real bridge ports

| N sandboxes | 1 | 50 | 100 | 200 | per-sandbox-table control, N=100 | unpoliced |
|---|---|---|---|---|---|---|
| guest throughput (Gbit/s) | 22.19 | 23.52 | 23.05 | 22.95 | 7.99 | 24.68 |

Control plane at N=200: install one more 405 ms, remove one 220 ms, `nft list table` 74 ms. Install cost is O(N)
(whole-table read-back); acceptable at the measured sizes, a candidate for an incremental read-back later.

## 6. Residuals and unverified (disclosed, for the Judge)

1. No background timer: a table or tc loss is detected at the next `exec`, not at once. Layers 1-2 contain meanwhile.
2. `nft flush ruleset` was not run literally; `delete table` was (same effect on the table, not on other tables).
3. The tc layer is stateless and passes ICMP type 3 to the guest; stateful rules exist only in layer 3. Layer 2 is an
   addition beyond the owner's stated direction (one table plus port hardening).
4. Requirements: nft >= 1.0.9 (`destroy`), kernel >= 6.3, tc flower, `bridge` tool. Not tested on older hosts.
5. Bridge conntrack (`nf_conntrack_bridge`) is a global side effect of loading a bridge ct rule. `br_netfilter` was not
   exercised.
6. Precondition: a bridge dedicated to Kata (default-drop covers every port of `ibrname @bridges`). Stale entries after a
   backend crash persist until the next sweep (any install) -- the port flags and tc keep them contained.
7. A hostname grant means the create-time A-record set; later DNS changes do not widen or narrow it. A NAT-hairpin WAN
   address of the host cannot be detected as host-local.
8. `deny_all` plus a non-empty allowlist semantics follow the existing validation; unchanged here.
9. Scale was measured with netns guests, not 200 microVMs. My tests had no real internet path except E19.
10. The M8 first-run zero-byte download flake (R15) was not reproduced and is unexplained.
11. Install is O(N) in control-plane time (section 5).

## 7. What the Judge should challenge

Whether layer 2 (tc) is worth its extra surface; whether exec-time-only detection is enough or a timer is required;
the net_bytes silent-stall contract; the nft/kernel version floor; the dedicated-bridge precondition; the hostname
grant semantics; and whether E19's need for operator FORWARD accepts should become a documented host prerequisite.
