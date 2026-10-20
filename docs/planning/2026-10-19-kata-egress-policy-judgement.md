# Kata egress policy: judgement (ADR-240, GitHub #194)

Stage: JUDGE of design -> red-team -> prove -> judge. Date: 2026-10-19/20. Judge branch `kata-egress-judge`, cut from
`kata-egress-prove`. Claim under judgement: `decisions/ADR-240-kata-egress-policy-enforcement-point.md` (status Proposed).
The previous stage's summary was not trusted; everything below was re-derived from the code and re-run on `thnak-server`
(Ubuntu 26.04, kernel 7.0.0-38, nftables 1.1.6, iproute2 6.19, containerd 2.4.2, Kata 4.1.0 runtime-rs/cloud-hypervisor,
gcc 15 Release). I changed no implementation or ADR file; all mutations lived only in the server copy of the sources and
were restored (server sources diffed byte-identical to the originals before and after; `git status` of this worktree is
clean of mutations).

## 0. Verdict

**judge-ready-after-listed-changes.** No blocker was found: I could not make a hostile guest (root, CAP_NET_RAW) reach
anything outside its exact `(ip, tcp port)` set, and every fail-closed ordering claim held. But the shipped tests do not
prove three things the ADR states as fact (the default drop for unregistered bridge ports; the native port flags as an
independent layer; the version-floor fail-closed behaviour), and several honest-residual items are missing from the ADR.
Section 8 lists exactly what must change first.

One-line findings:

* FAILED-as-tested (claim is true by my own probe, but the shipped tests stay green when it is removed): layer 3's default
  drop for a bridge port with no map entry (mutants M13, M14); layer 1's port flags `learning off` / `locked on` / `isolated on`,
  singly or all together (M10, M11, M12, M15, M16 leave `egress_linux` 73/73 green).
* Design claim "older nft/kernel fail create closed" is wrong by code reading: only the REMOVAL script uses `destroy`, so
  on nft < 1.0.9 create succeeds and teardown/sweep fail forever (stale state accumulates, bridge stays default-dropped).
* The "dedicated bridge" precondition is unenforced: any bridge the CNI result names is added to `bridges` and every
  unregistered port on it, plus host-originated traffic out of it, is dropped.
* ADR residuals are incomplete (section 7); I4 is listed as an upheld invariant with no mechanism or test.
* Test hygiene: three suites return early on a failed precondition leaving the lab (and a sink holding ctest's pipe, a
  908 s hang under ctest); the lifecycle suite leaks one CNI ipMasq rule into the host's nat table per run.

## 1. Independent re-run (question 2)

Build: `bash tools/ssh-linux-build.sh` (sync) then, in `~/ae/build-gcc-kata`: `cmake` re-run, ninja of the kata targets
(the sync script's `AE_TARGETS` handling breaks on more than one target; I built by hand), and
`sudo -n ctest -R "kata_backend|egress" --output-on-failure --timeout 300`. clang (`build-clang-kata`): the six
kata/egress test targets build with `-Werror`, `test_kata_egress_policy_render` ALL PASS.

Prerequisite I had to add: `docker.io/library/python:3.12-alpine` was NOT present in containerd on the server (every
backend create in the first run failed `ctr images mount ... not found`). I pulled it (`ctr images pull`; benign, additive).

| Run | Result |
|---|---|
| ctest, scoped FORWARD accepts present (`ae-judge-temp ae-prove-temp`) | **11/11 passed**: net_egress_proxy, https_egress, provider_egress_address_policy, render, kata_backend_linux 6 s, slice2 40 s, abuse_corpus 41 s, slice9_10 33 s, egress_linux 193 s, lifecycle 100 s, scale 80 s |
| the three egress binaries run directly, same accepts | egress_linux **73 ok / 0 FAIL**, lifecycle **119 / 0**, scale **27 / 0** (matches the ADR's 73/119 exactly; scale numbers: N=1 21.01, N=50 22.98, N=100 23.96, N=200 22.27 Gbit/s; per-sandbox-table control at 100: 7.93 vs unpoliced 25.33; N=200 install 423 ms, remove 253 ms, list 81 ms) |
| render unit test | 102 ok, ALL PASS |

Accept rules: inserted by a wrapper that removes them in the same command via `trap EXIT`; six rules
(`aekata0<->aepvw0`, `aepvbr0<->aepvw0`, `aekata0<->enp7s0`), comment `ae-judge-temp ae-prove-temp` (the tests grep the
`ae-prove-temp` marker, so the comment carries both). After every run the count was verified: 0.

### Which checks depend on the FORWARD accept, and what happens without it

Run with the image present and NO accepts (binaries run directly):

| Binary | Result without accepts | Loud or silent |
|---|---|---|
| egress_linux | 54 ok / **19 FAIL** | loud. The precondition `check()` fails first; then every check needing world connectivity fails: CONTROL reach of 9000/9001/.3, conntrack-flood control (0 forwarded), inbound control, ALLOW, per-sandbox ALLOW, post-flood connect, same-sandbox outbound, PMTUD (both), no-net_bytes control, all six net_bytes checks |
| lifecycle | 110 ok / **9 FAIL** | loud: precondition, CONTROL, T1 baseline, T1 allowed-after-loss, T1 CONTROL, T2, T6 A and B, T8 |
| scale | 1 ok / 1 FAIL | loud (precondition) but it `return 1`s before `lab.down()`: the sink process keeps ctest's output pipe open and ctest hung 908 s in the first run; the lab veth/netns stay until the next `lab.up()` |
| slice9_10 | 16 ok / **2 FAIL** | case 6 "allowed example.com:443" and case 9b positive control fail loudly. Case 9 (cut off by the quota) and the case-6 "1.1.1.1 blocked" check PASS VACUOUSLY without egress; they are only protected by 9b and the case-6 allowed check in the same suite |

Checks that do NOT depend on it (host-INPUT and L2 checks: host sshd, IPv6 link-local, guest-to-guest, hostile frame
matrix, FDB/ARP poisoning, table-loss containment toward the host) passed without the accept; those negatives have
controls that also pass without it. Nothing is silently skipped when the accept or a tool is missing: the failure mode is a
loud, slow, leaky one (see S12).

## 2. Claim vs test audit (question 1)

Method for each ADR-240 §4 row: read the test, decide whether its probe can fail and whether its control exercises the same
probe against the unprotected state; mutate. 16 mutants were applied to the server copy of the implementation (one at a
time, rebuild, run `render` + the relevant live binaries under the scoped accepts, restore). Mutation numbers: `ok/FAIL`.

### 2.1 Mutation table

| # | Mutation (file) | Render unit | egress_linux | lifecycle | Killed by |
|---|---|---|---|---|---|
| M1 | `in_<tag>` final `drop` -> `accept` (render) | 102/0 | not run | 118/**1** | T2 hostile batch: GRE, ICMP, IPIP, UDP, SPIP, ARPSP delivered. Connect probes stayed GREEN (host sshd "blocked") because `out_` still drops the reply: a one-way hole only the sniffer sees |
| M2 | `HostEgress::verify` returns OK (host) | 102/0 | not run | 113/**6** | T1 x3, T2, T3, T4 exec-refusal checks |
| M3 | sweep treats every port as dead (host:174) | 102/0 | not run | 112/**7** | T1 baseline, T6 B, T8 x2, T10 x2, T11 |
| M4 | `out_` chain loses the quota rule (render) | 102/**0** | 69/**4** | not run | download counted-inbound, quota used, shared budget x2. The render unit test does NOT catch it |
| M5 | return path without `ct state established,related` (render) | 100/2 | 72/**1** | not run | "unsolicited inbound ... dropped" (INBOUND-CONNECTED) |
| M6 | tc layer never installed (host:265) | 100/2 | not run | 109/**10** | T1 (host sshd and unlisted IP reachable after table loss, hostile batch leaks), T7 inject-tc "UNEXPECTED SUCCESS", T10 |
| M7 | SYN rate limit removed (render) | 101/1 | 72/**1** | not run | conntrack flood: 20000 of 20000 forwarded (+20000 conntrack) |
| M8 | static FDB install skipped (host:251) | 101/1 | not run | 41/**20** | every create fails read-back ("static FDB entry missing"); red through install verification, not through behaviour |
| M9 | `ether type != { ip, arp }` rule removed (render) | 101/1 | not run | 118/**1** | T2 batch: VLAN=3 delivered |
| M10 | `learning off` removed (set + verify) | 101/1 | 73/**0** | 118/1 | only T4's flag-text assertion; behaviour tests green |
| M11 | `locked on` removed | 101/1 | 73/**0** | 118/1 | same |
| M12 | `isolated on` removed | 101/1 | 73/**0** | 118/1 | same |
| M13 | default drop for unregistered ports (`meta ibrname @bridges drop` in `c_input`) removed, render and matcher consistent | 101/1 (canned listing only) | 73/**0** | 119/**0** | **nothing live** |
| M14 | `c_forward` port drops removed, consistent | 101/1 (canned listing only) | 73/**0** | 119/**0** | **nothing live** (masked by isolated + tc + `in_`) |
| M15 | `learning off` + `locked on` removed (`isolated on` stays) | 101/1 | 73/**0** | not run | nothing behavioural |
| M16 | ALL port flags removed (set + verify) | 101/1 | 73/**0** | 116/**3** | T4 flag-text, T4 exec-detects-reset, T7 inject "bridge link set" only; FDB poisoning, source-MAC spoof and guest-to-guest tests all stayed green |

Weakened-then-restored: server `~/ae/src/src/backends/kata/*.cpp` diffed identical to the pre-mutation copies after the
last restore, and the kata libraries and test binaries were rebuilt pristine before the probes of §3.

Why M10-M16 survive (this is the substantive result): the layers overlap. With tc present, a spoofed source MAC is dropped
at tc ingress before the bridge learns it, so the FDB-poison test is satisfied without the flags; with `in_`/tc present,
guest-to-guest and unregistered-port frames are dropped anyway. The tests prove the union, not each layer, while the ADR
(§2) states per-layer properties ("survives any nft flush", "closes spoofing, FDB poisoning, guest-to-guest").
`T1`/`T2` do remove layers, but their hostile batch is only sniff-based: T2 (nft + flags remain) never polls the FDB, and T1
(tc + flags remain) never repeats the FDB or peer probe with tc gone. I therefore ran the missing independent proofs myself
(§3.1, §3.2). Both claims are TRUE; they are just not in the suite.

### 2.2 Per-claim table

Verdict key: VERIFIED / VERIFIED-WITH-CAVEAT / FAILED / UNVERIFIABLE. "FAILED" is applied per the brief's rule (a claim whose
test stays green under its own mutation), and flagged where my own probe shows the claim itself is true.

| Claim (ADR row) | Test (file:line) | Can it fail? | Mutation result | Verdict |
|---|---|---|---|---|
| E1 allowlisted (ip,port) reachable, same IP other port / unlisted IP blocked | `egress_linux` ALLOW/DENY 285-294 | yes: the identical probe run in the unpoliced control reaches all three (235-237), and the allow and the denies share one guest python process | masked by tc+nft overlap (M1 alone not detected by this test; T2 detects it); M6+nft-only exercised in T2 | VERIFIED |
| E2 host sshd (gateway + LAN IP), v6 link-local, guest-to-guest blocked | `egress_linux` 291-294 | yes: control reads SSH banners x2, v6, peer (238-241); without FORWARD the control still passes (INPUT path), so it does not depend on the lab | T1 control (table and tc gone) reads the banner; M6 red | VERIFIED. Caveat: the connect probe cannot see a one-way hole (M1 stayed green on connect probes) |
| E3 hostile frames not delivered (13 classes) | `egress_attack_kit.hpp`, run at `egress_linux` 305-323 and lifecycle T1/T2 | yes: the control delivered all 13 classes (my run: FRAG1/FRAG2/VLAN/UDP/SPIP/SPMAC/ICMP/IPIP/GRE/IPV6/RAWETH/ARPSP/FDBS all >0) and LEGIT/ARPOK show the sniffers and sender work; a failed sniffer (empty map) would be caught by LEGIT>0 | M1 and M9 red via T2; M6 red via T1 | VERIFIED |
| E4 spoofed MAC does not poison a neighbour's FDB; forged ARP does not poison host neighbours | `egress_linux` 324-331 | yes: control shows victim behind two ports and the poisoned neighbour entry | M10-M12, M15, M16 all green: the test is satisfied by tc+nft+static FDB without any port flag | VERIFIED-WITH-CAVEAT (layer 1 independence FAILED as tested, TRUE by judge probe §3.1) |
| E5 conntrack flood bounded | `egress_linux` 333-341 | yes: control 20000 forwarded | M7 red (20000 forwarded) | VERIFIED. Caveat: SYN-only; non-SYN floods are not limited (§3.3) |
| E6 fragment-memory flood bounded | `egress_linux` 342-346 | control peak 4.2 MB vs 0 B | not mutated (c_pre -450 drop and tc `nofrag` overlap; both would need removal) | VERIFIED-WITH-CAVEAT (no mutation run) |
| E7 unsolicited inbound dropped | `egress_linux` 348-354 | yes: control connects | M5 red | VERIFIED |
| E8 PMTUD works | `egress_linux` 361-391 | yes: the test deletes the related-ICMP rule and the same upload stalls | in-test control | VERIFIED |
| E9 net_bytes counts both directions, one shared budget | `egress_linux` 393-444 | yes: no-quota control moves 1,000,000 both ways | M4 red (4 checks) | VERIFIED |
| E10 table loss still contains the guest | lifecycle T1 205-254 | yes: tc removed too -> host sshd and unlisted IP reachable | M6 red (10); my literal `nft flush ruleset` in a private netns (§3.4) | VERIFIED |
| E11 exec-time verification detects loss and tamper, kills, refuses | lifecycle T1, T3, T4 | yes: untampered exec runs (T3 control) | M2 red (6) | VERIFIED |
| E12 tc removed: nft + flags alone contain | lifecycle T2 256-281 | yes: both removed -> reachable | M1, M9 red (T2) | VERIFIED. Caveat: "+ flags" contributes nothing the test can see (M16) |
| E13 teardown leaves nothing | lifecycle T5/T6 world-compare 90-107 | yes: T7 control proves the compare sees a held create | not mutated | VERIFIED-WITH-CAVEAT: the compare ignores host iptables-nat; the lifecycle suite leaks one CNI ipMasq POSTROUTING rule + chain per run (T8's deliberate `ip netns delete` makes `cnitool del` fail). I removed 14 such rules I leaked this session; 5 pre-existing ones from earlier stages remain untouched |
| E14 parallel create/probe/destroy, one backend | lifecycle T6 346-379 | yes (each probe also asserts the other tuple blocked) | M3 red | VERIFIED. I added two concurrent backend PROCESSES, 3 rounds: all passed, 0 tables after each (§3.5) |
| E15 sweep keys on the port, not `/var/run/netns` | lifecycle T8 424-461 | yes: the dead-port entry IS swept in the same run | M3 red | VERIFIED |
| E16 install fails closed at every step | lifecycle T7 + render scripted runner | yes: M6 makes the inject-tc case "UNEXPECTED SUCCESS" | M6, M16 red in T7 | VERIFIED |
| E17 host firewall unchanged by a full run | `egress_linux` 446-460 | yes | not mutated | VERIFIED-WITH-CAVEAT: egress_linux only (the lifecycle leak above is outside this check); `nf_conntrack_bridge` loads as a global side effect (disclosed) |
| E18 A-record cap, host-local rejection, hostile strings refused | render 160-200, lifecycle T11 | pure unit strings + live T11 (12 A records -> 8; 3 -> 3; host-local rejected) | M3 made T11 red as a side effect; cap itself not mutated | VERIFIED. `resolve_and_validate_all` has no direct test (grep finds none under `tests/`) |
| E19 real internet path via the backend | slice9_10 cases 6, 9, 9b | case 9 can pass vacuously (see §1) but 9b cannot | not mutated | VERIFIED-WITH-CAVEAT: needs operator FORWARD accepts and the CNI ipMasq; without them fails loudly via case 6 and 9b |
| (not tabled) layer 3 default drop for any port of the CNI bridge with no map entry (ADR §2) | none live; render unit only checks `base_table_matches` text | no: M13 leaves 73/73 and 119/119 green | **FAILED as tested.** TRUE by judge probe §3.2 | FAILED (claim true) |
| (not tabled) layer 1 flags `learning off`, `locked on`, `isolated on`, static FDB: independent, flush-proof layer | none isolates them | no: M10-M12, M15, M16 green | **FAILED as tested.** TRUE by judge probe §3.1 | FAILED (claim true) |
| (not tabled) design 6.3 "older nft/kernel fail create closed" | none | n/a | by code reading wrong (§5, S3) | FAILED (claim false) |

## 3. Judge probes (extra evidence, not in the shipped suite)

Run as throwaway binaries linked against the real backend library (sources kept on the server in `~/aejd/`, not committed).

### 3.1 Layer 1 is independently load-bearing (the missing proof)

Two real policy sandboxes; nft table deleted and the tc qdisc removed on both ports; then FDB-poison probe + peer connect +
host sshd, flags first in place, then reset (`learning on locked off isolated off`):

```
flags=ON : victim MAC seen behind {its own port only}; peer=FAILED OSError;           gw22=CONNECTED SSH-2.0-OpenSSH_10.2p1
flags=OFF: victim MAC seen behind {victim port AND attacker port}; peer=CONNECTED PEER-OK; gw22=CONNECTED SSH-2.0-OpenSSH_10.2p1
```

So with nft AND tc gone the flags alone stop FDB poisoning and guest-to-guest, and (as the design says) do nothing about the
gateway/host sshd. Layer 1 works; the suite just cannot show it.

### 3.2 Default drop of an unregistered bridge port

Private network namespace, real `HostEgress` policy for guest 1, plus a second guest on a bridge port with no map entry,
no tc, no flags: it cannot reach the world sink or a host service (`FAILED OSError` both); after deleting only the two
default-drop rules (`c_input`, `c_output`) the same probe reaches both (`CONNECTED WORLD-OK`, `CONNECTED HOSTSVC-OK`).
The claim is true and the control proves the probe can see it. This also demonstrates the collateral effect of S2.

### 3.3 Non-SYN traffic to the allowed tuple

20000 raw frames to the allowed tuple, per TCP flag value, forwarded to the world side: SYN 104 (limit holds), ACK 20000, RST
20000, FIN+ACK 20000, PSH+ACK 20000. Global conntrack delta stayed ~0 for the non-SYN floods on this host
(`nf_conntrack_tcp_loose=1`, Linux 7.0), so R12's conntrack-growth claim holds as measured; but the SYN limit is the only
packet-rate bound, and a guest may send arbitrary stateless TCP segments to its allowed tuple at line rate (bounded only
by `net_bytes` when set). It stays inside the grant (same destination, same port); recorded as accepted-residual S9.
Also: allowed-tuple frames addressed to broadcast or multicast MACs reach the bridge device (host drops them, nothing
forwarded: world=0), and to a peer MAC reach nowhere; the tc layer has no `dst_mac` match although design 6.1 says
"(guest mac, bridge mac, ...)".

### 3.4 Literal `nft flush ruleset`

Done in a private network namespace (`ip netns add aejd-host`, the whole probe run inside it; the program refuses to run if
it can see the host's NIC or docker/ufw tables), with the same table layout installed by the real `HostEgress::install` for
a netns+veth lab guest behind a real bridge port, world sink, and a "host service" on the gateway address. The host's real
ruleset was never touched (it holds Docker, ufw, fail2ban tables).

```
control (no policy): other port, host service reachable
policy:              allowed tuple works; other port / unlisted IP / host service blocked (timeouts)
nft flush ruleset  -> `nft list ruleset` empty
after flush:         allowed tuple WORKS (tc); other port, unlisted IP, host service STILL blocked;
                     hostile batch (frags, VLAN, UDP, spoofed IP/MAC, ICMP, IPIP, GRE, v6, raw ethertype, ARP spoof, FDB spoof): all 0
HostEgress::verify:  detects ("the shared nftables table bridge ae_k is gone")
control: flush + `tc qdisc del`: host service and unlisted IP reachable
```

What contains the guest after a literal flush is tc ingress/egress plus the bridge port flags: tc is not nft and `flush ruleset`
does not touch it. NOT done: the literal flush against a running microVM. Reason: a Kata guest needs `/var/run/netns/<id>`
visible to containerd's shim, which an `ip netns exec` private mount namespace hides, and a flush on the host's main namespace
would delete the other tenants' Docker/ufw/f2b rules. The microVM/`delete table` case is the shipped T1 (passes, and M6 shows
it can fail). Datapath for a lab guest and a microVM is identical at the host veth end (frames cross it either way).

### 3.5 Two backend processes, and a backend crash

* Three rounds of two concurrent `KataBackend` processes (entries `.2` and `.3`): all created, each reached only its own
  tuple, gw22 blocked, all destroyed, 0 `ae_k` tables after each round. The advisory `flock` works across processes.
* Crash: a process created a sandbox and `_exit`ed without destroy. The guest kept running; the table, vmap entries and
  `ports` element stayed; the orphan stayed contained (allowed tuple only). A second process's create+destroy ran the sweep
  twice and left the orphan's entry intact (its port exists), table kept (still default-dropping). I then cleaned the orphan
  by hand (task/container, overlay + tmpfs + snapshot unmounts, workdir, CNI del, netns delete, table). So stale entries after
  a crash are safe (fail closed), and are removed only after the port is gone, as designed. A crashed backend's orphan VM
  itself is a pre-existing resource leak, unchanged.

## 4. Security review (question 3)

Attacker: hostile guest (root, CAP_NET_RAW/CAP_NET_ADMIN inside the microVM). The guest cannot touch host netfilter, tc or
bridge flags at all; what it controls is the frames it emits and its own MAC. The host-side threat model that makes layers 1-2
necessary is host collateral (an ufw/firewalld/`/etc/nftables.conf` reload doing `flush ruleset`), which §3.4 exercises.

**Injection / I3.** Every value reaching `nft`, `tc` or `bridge` is typed or charset-checked: MAC `valid_mac` (lowercase hex,
unicast, nonzero; render.cpp), ifname `valid_ifname` (alnum `_-.`, <= 15, not leading `-`/`.`; excludes `*`, which nft treats
as a wildcard), tag `valid_tag` (alnum `_-`, <= 64), IPv4 as `uint32` via `ipv4_dotted`, ports `uint16`, bytes `uint64`;
`validate()` runs inside every generator, and removal/sweep generators re-validate (`render_remove_script`, host.cpp:165 sweep
re-checks tags and ifnames parsed back from nft). The tag is the backend's own `fresh_id("ae-kata")`, the veth/bridge names
come from sysfs/CNI JSON validated by `locate_port`, MAC and IP come from the CNI result and are re-validated
(`parse_cni_result`, `endpoint_sane`). Hostnames are resolved only by backend code from a host-authored, grant-covered entry
(kata_backend.cpp:250-289). No guest, DNS answer or model-derived string is spliced into a command or ruleset. Commands are
`posix_spawnp` argv vectors (no shell); the nft/tc input is a file in a 0700 root-owned directory removed after use. Residual:
tool names (`nft`, `tc`, `bridge`, `ip`) resolve through the backend process's `PATH` (S11).

**TOCTOU install vs verify.** Between `cnitool add` and install the port exists unpoliced but no guest is running (policy is
installed before `ctr run`; the red-team's R2 ordering holds). Verify at exec compares against an install-time snapshot, so it
detects change, not correctness: if a step silently installed nothing (an empty tc listing, a base table not containing the
expected sandbox rules) the snapshot would record that and verify would pass forever. The live tests catch it behaviourally;
install does not compare the snapshot to the expected render (S8).

**Partial install / teardown.** Create: flags -> FDB -> tc (default-deny first) -> base ensure -> one sandbox transaction ->
read-back; failure after the transaction commits runs `undo` (`remove_locked`), failure before leaves only more-restrictive
partial state which `cleanup_partial` removes with the veth. T7 injects at six steps; M6/M16 show T7 can fail. Destroy keeps the
policy if the port persists (`egress_policy_kept`, T8/T9). Verified.

**Two processes / crash.** §3.5. The lock is `/run/agentengine-kata/egress/lock` (0700 dir); 30 s acquisition timeout fails
create closed. Verify is deliberately unlocked (nft list is atomic). A veth-name collision between a stale entry and a new
sandbox would fail the new install on duplicate-element (fail closed); names are CNI-random so this is not reachable in practice.

**Ownership / sweep.** The sweep removes only entries whose veth is absent from `/sys/class/net` plus chains no map element
references; M3 shows the tests detect a wrong ownership test; the orphan and netns-path cases held. It can only ever touch
objects inside `table bridge ae_k` and the `ports`/`bridges` sets of that table. It never runs iptables, ufw or Docker
commands; a literal read of both host files finds no such call. It does, however, take over any bridge the CNI result names
(S2).

**Resource exhaustion.** Per sandbox: 2 chains, 2 map elements, <= ~70 rules (cap 8 A-records per entry, 64 total; tc prefs
20..83 stay below the pref-100 default drop), measured to 200 sandboxes (flat 22 Gbit/s; install 455 ms at #200, O(N)
whole-table read-back). 30 s lock timeout bounds a parallel-create storm (fails closed). The number of sandboxes is not itself
capped by this layer.

**DNS.** Resolution once at create, cap and dedupe, blocked ranges filtered (`is_blocked_address`), host-local addresses
(`getifaddrs`) rejected, everything fails closed on error. A name's records beyond the first 8 are not reachable (functional,
resolver-order dependent). Rebinding is not possible (no later DNS), but the host-local check is also create-time only (S4).

**net_bytes contract.** One named quota per sandbox, both directions, packet-whole drop, silent stall, dies with the table:
confirmed by M4 and E9. In degraded mode (table lost, before the next exec) the quota is void while tc still allows the tuple (S5).

### Findings, with priority

| # | Finding | Priority |
|---|---|---|
| S1 | Shipped tests do not prove layer 3's default drop for unregistered ports, nor layer 1's flags as a layer (M13, M14, M10-M12, M15, M16 survive). Both true by §3.1/§3.2 | must-fix (add two small tests with positive controls, or narrow the ADR text) |
| S2 | Dedicated-bridge precondition unenforced: `install` adds whatever `/sys/class/net/<veth>/master` says to `bridges`, so a CNI network pointed at a shared bridge (docker0, a LAN br0) default-drops every foreign port on it and all host-originated traffic out of it (`meta obrname @bridges drop`) | must-fix (cheap guard: refuse a bridge that has ports other than backend-created veths / is not the configured CNI bridge, or at least log and document) |
| S3 | `nft` >= 1.0.9 / kernel >= 6.3 floor not preflighted; only `render_remove_script` uses `destroy`. On older hosts create succeeds and every remove/sweep fails, so entries never clean up and the bridge stays default-dropped; design 6.3 claims "older versions fail create closed" | must-fix (preflight `nft --version` / a `nft --check` of a `destroy` line at first install, or amend the claim) |
| S4 | Host-local check is create-time only: an allowed address later assigned to the host (VPN, DHCP, a new Docker network) turns a granted tuple into a grant over host services through INPUT; NAT-hairpin WAN address undetectable (disclosed) | accepted-residual (add the "acquired later" case to the ADR) |
| S5 | Degraded mode (table lost, before the next exec) loses: the quota (I8 `net_bytes` void), conntrack statefulness (unsolicited inbound from allowed sources possible), SYN limit, -450 fragment drop. ADR residual 1 says only "layers 1-2 contain" | must-fix as documentation; no code needed (and see §6) |
| S6 | `resolve_and_validate_all` header says it "shares an implementation" with `resolve_and_validate`; it duplicates the filter loop (shares only `resolve_candidates`/`is_blocked_address`), and has no direct test | accepted-residual / doc fix |
| S7 | Misleading text left behind: `kata_backend.hpp` points to "ADR-240 section 8" for residuals (they are section 6); the SLICE 10 header block (lines ~243-250) still describes the netns nft policy as current without an inline pointer; the new header lines 338 and 390 contain invalid UTF-8 bytes where `§` was written; design 6.1 describes a tc `bridge mac` match that is not implemented | doc fix |
| S8 | Verify compares to the install-time snapshot; install never checks the snapshot content against the expected render | accepted-residual (should-fix: assert expected rule counts at install) |
| S9 | Non-SYN TCP segments to the allowed tuple are unrate-limited (20000/20000 forwarded; no conntrack growth measured with tcp_loose=1); ARP to the gateway is unrate-limited and unmetered (red-team accepted it, the ADR omits it); wire bytes can exceed N by header replication (2-6%) and a GSO packet up to 64 KiB; silent stall gives the host no "budget exhausted" signal | accepted-residual (add to ADR) |
| S10 | ICMP type 3 is passed by tc stateless; only layer 3 ties it to a flow (ADR residual 3, confirmed) | accepted-residual (disclosed) |
| S11 | Tool names are PATH-resolved; install writes tc/nft batch files with `std::ofstream` under the 0700 dir | accepted-residual (host-owned environment) |
| S12 | Test hygiene: egress_linux/lifecycle/scale return early on a failed precondition leaving the lab; scale left a sink holding ctest's pipe (908 s hang); lifecycle T8 leaks a CNI ipMasq rule per run (14 leaked in this session, cleaned); T11 edits `/etc/hosts` temporarily (cleaned on normal exit only); `forward_precondition_met` accepts only the `ae-prove-temp` marker, so a host whose FORWARD is already open still "fails"; the suites need `python:3.12-alpine` pre-pulled (it was absent) | must-fix (cheap) |
| S13 | I4 ("every effect is attributable") is listed in ADR-240's header with no mechanism or test: nothing emits an audit record for install/remove/kill beyond stderr lines; chain/object names carry the backend id (state attribution only) | must-fix (reword the ADR or add an audit hook; ADR-070's "always audited" is the seam this is claiming) |

No blocker: nothing lets a hostile guest widen its own egress, and every failure path I exercised (create faults, port
persisting, table loss, tamper, flag reset, crash, concurrent processes) failed closed.

## 5. Invariants (question 4)

I2 (no ambient authority): every allowed tuple comes from `spec.net.allowlist`, and a network is only enabled when
`wants_network` (`!deny_all || !allowlist.empty()`) AND `sandbox_net_out_grants()` is non-empty (kata_backend.cpp:202-204);
`authorize_spec()` has already rejected entries not covered by a grant, and the unrestricted shape (`deny_all=false`, empty
allowlist) is rejected by the grant gate and by `authorize_spec` before any policy code runs. An empty `allowed` list cannot
install (`validate()` requires 1..64). `deny_all=true` with no allowlist creates no netns, no NIC, no port, no table
(T5: guest has only `lo`). I found no path to guest egress without a grant. Hostname grants expand to up to 8 records
resolved at create (S4/§4 DNS).

I3: nothing derived from model or guest output reaches a generator (§4). The only strings of external origin are the CNI
plugin's JSON (host-authored config), re-validated.

I8: `net_bytes` enforced (M4), with the degraded-mode gap (S5) and no exhaustion signal (S9).
I4: see S13.

## 6. tc layer, detection model (question 6)

**Is the tc layer worth its extra surface? Yes, keep it.** It is the only layer that contains the guest toward the gateway,
host sshd and unlisted IPs once the nft table is gone (§3.1: flags alone leave gw22 open; M6 turns T1 red in 10 places). It
survives a literal flush (§3.4) because it is not nft. Surface cost: ~25 generated tc lines per sandbox, a `tc` + `cls_flower`
dependency, a stateless ICMP type-3 pass, snapshot-equality verification of `tc filter show`, no `dst_mac` match. It is an
addition beyond the owner's stated direction (ADR residual 3); the owner should record the approval in the ADR.

**Exec-time-only detection: acceptable, with changes; a mandatory watchdog is not required.** Containment (the property that
matters for I2) does not depend on detection: layers 1-2 hold without the table (§3.1, §3.4, T1). What detection protects is
the quota and statefulness (S5). A background thread in the backend adds a lifecycle the session model (I1) does not have.
Cheaper and in line with ADR-070's delegated-seam shape: enumerate the degraded set in the ADR (S5), and add a public
`verify_egress(handle)` (reusing `HostEgress::verify`) so a host that wants a timer can call it. If the owner treats
`net_bytes` as a security budget rather than a cost guard, a timer becomes required.

## 7. Honesty review of ADR-240 (question 5)

Accurate: the problem statement; the supersede banners on ADR-093/095 (they say exactly which parts are superseded and what
stays; the original text is kept as the record); the evidence table counts (73/119/27 and the scaling numbers reproduce); the
scale control; the status `Proposed`.

Overclaims or omissions:

1. §2 states per-layer properties ("survives any nft flush", layer 1 "closes source-MAC spoofing, FDB poisoning,
   guest-to-guest", layer 3 "default DROP for any port of the CNI bridge without a map entry") that the tests show only
   for the union (S1). All true by my probes; say "verified by judge probes" or add the tests.
2. §4 E12 "tc removed: nft + flags alone contain" and E10: the flags contribute nothing the test can see (M16).
3. Residual 1 says layers 1-2 "contain meanwhile" and does not list what is lost (S5).
4. Residual 4 "requirements ... not tested on older hosts" is honest, but design 6.3's "older versions fail create closed" is
   false (S3) and ADR §6 does not warn that removal breaks.
5. Residuals missing relative to the red-team's own accepted list: unrated/unmetered ARP to the gateway; wire bytes vs L3
   bytes (2-6%, 64 KiB GSO crossing); 5-day established-conntrack linger; the create-time-only host-local check; the leaked
   `cnitest-*` shim namespaces (pre-existing); tc without `dst_mac`; non-SYN traffic unrate-limited (§3.3); and the
   dedicated-bridge collateral (S2).
6. The header lists I4 as upheld with no evidence (S13).
7. The previous stage's listed residuals (tc extra surface, detection only at exec, `delete table` not `flush ruleset`, tc
   passes ICMP type 3, nft/kernel floor, bridge conntrack global side effect and br_netfilter untested, dedicated bridge,
   stale entries after crash, NAT hairpin, net_bytes stall, unexplained 0-byte download) are all present in ADR §6. The
   "`delete table` not a literal flush" item is now closed by §3.4 (private netns, lab guest); the 0-byte download I could not
   reproduce or explain and did not investigate.
8. Corrections to ADR-093/095 are accurate, but incomplete in two places: the SLICE 10 header block in `kata_backend.hpp`
   (lines ~243-250) still describes netns nft enforcement as current without an inline pointer (S7), and
   `docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md` still shows the netns `ae_netpolicy`
   commands as the design. (I grepped `src/`, `rfcs/008*`, ADR-08x/09x, `docs/*.md`, README and the specification for other claimants and found only
   those; I did not read every RFC end to end.)
9. The previous stage deleted `docs/planning/2026-10-19-session-state-kata-egress-and-recycling.md` (162 lines) in the same
   change set; unrelated to the claim, but the owner should know it left the tree.

## 8. What must change before the owner marks ADR-240 Judged

Must (small):
1. Add live tests with positive controls for (a) default-drop of an unregistered bridge port, and (b) layer 1 alone
   (nft + tc removed, flags on vs off: FDB poison + peer), or narrow the ADR's per-layer wording. M13/M10-M16 must go red.
2. Preflight the nft/kernel floor so older hosts fail create closed (S3), or correct the claim and §6.
3. Guard or loudly refuse a non-dedicated bridge (S2); document the collateral if kept.
4. ADR text: enumerate degraded-mode losses (S5); add the missing residuals (section 7 item 5); reword or evidence I4 (S13);
   fix the pointer (`section 6`, not `8`), the stale SLICE 10 header block and the invalid bytes, the `bridge mac` description (S7);
   record the owner's acceptance of the tc layer.
5. Test hygiene (S12): run `lab.down()` on every exit path; have lifecycle T8 `cnitool del` before `ip netns delete`; document the
   `python:3.12-alpine` prerequisite and the FORWARD-accept prerequisite for E19 as host requirements.

Should: `verify_egress` public hook (§6); install-time snapshot content check (S8); a direct unit test of
`resolve_and_validate_all`.

## 9. Could not verify, and why

* A literal `nft flush ruleset` against a running microVM (§3.4): see there; done in a private netns with a lab guest instead.
* nft < 1.0.9 / kernel < 6.3 behaviour (S3 is by code reading); kernels other than 7.0.0; `br_netfilter` loaded.
* 200 real microVMs (scale used netns lab guests, as the ADR says).
* Mutation of E6 (fragment floods) and E18's cap constants: two overlapping layers / pure strings; not run.
* The unexplained first-run zero-byte download: not reproduced.
* Real multi-A DNS and a NAT-hairpin WAN address: no such name/topology on the host (T11 uses a temporary `/etc/hosts` block).
* Behaviour under a different host firewall (iptables-nft rule sets other than this host's Docker/ufw/fail2ban).

## Appendix: host state left behind and cleaned

Image `docker.io/library/python:3.12-alpine` pulled into containerd (kept). Temp FORWARD accepts: removed in the same
command every time, final count 0. Leaked CNI ipMasq rules created by the lifecycle suite during this session: 14 removed, the
5 that predate this session untouched. One crash-test orphan sandbox (task, mounts, workdir, CNI, netns, table) cleaned by hand.
Final check: 0 `ae_k` tables, 0 containers, 0 `ae-kata` mounts, 0 lab veths/netns, 0 `ae-*` FORWARD rules. All my objects
used `aejd-`/`aepv-`/`aejd-host` names; the private namespace `aejd-host` was deleted.
