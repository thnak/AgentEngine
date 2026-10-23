# KataBackend: first live verification, 2026-10-19

First execution of `test_kata_backend_{linux,slice2,abuse_corpus,slice9_10}_linux` against a real
Kata deployment. ADR-084 through ADR-095 disclosed these as compile-verified only; this is the
verification they asked for. Host: bare-metal Xeon E5-2680 v4, Ubuntu 26.04 (kernel 7.0.0), containerd
2.4.2, Kata 4.1.0 (runtime-rs, cloud-hypervisor), CNI plugins 1.9.1, gcc 15.2, Release build.

## Results as run

| Suite | Result |
|---|---|
| `test_kata_backend_linux` | pass |
| `test_kata_backend_slice2_linux` | pass |
| `test_kata_backend_abuse_corpus_linux` | pass |
| `test_kata_backend_slice9_10_linux` | 1 failure, plus the findings below |

Checks that really exercised the guest and held: capability gates fail closed, malformed and
loopback-resolving allowlist entries are refused, a disk write over `disk_bytes` gets a real `ENOSPC`
(with an in-budget positive control), pids and `--config-mode` creation.

## Finding 1 -- the network checks in slice9_10 could not fail (test defect)

The guest image (`busybox:latest`) has no `curl`. Cases 6 and 9 ran `curl ... || echo FAILED`, so
"the guest can reach the allowlisted destination" could never pass, and "cannot reach 1.1.1.1" and
"cut off by the net_bytes quota" passed because `curl: not found` prints the same `FAILED` as a blocked
connection. Nothing in the suite proved the allowlist or the quota enforces anything.

Fix made in this change (uncommitted at the time of writing): probes use busybox `nc` (TCP connect) and
`wget`; the allow/deny checks require `CONNECTED` / `FAILED` explicitly; a new case 9b is the positive
control for case 9 (same HTTPS request without `net_bytes` must succeed).

## Finding 2 -- the nftables allowlist does not filter microVM traffic (backend defect, security)

Reproduced by running the guest probe (`ctr tasks exec` into the running task) with the netns held open.

- Guest network: `eth0` 10.88.77.x/24, default via the bridge (10.88.77.1); in the backend's netns the
  VM attaches through `tap0_kata`.
- The backend's nft policy is one table, `inet ae_netpolicy`, with a single chain hooked on `output`
  (`policy drop`; accepts `lo` and `ip daddr <pinned> tcp dport 443`). An `output` hook only sees
  locally generated packets. The microVM's packets are forwarded at L2 between `tap0_kata` and `eth0`,
  so they never traverse it.
- With the host's `FORWARD` policy opened for `aekata0` (a temporary rule, since removed), the guest
  connected to the allowlisted address, to the unlisted `1.1.1.1:443`, and to `8.8.8.8:53` (all
  `nc` rc=0). Only an unrouted address failed.
- With the host's `FORWARD` policy left at `DROP` (Docker and ufw both leave it there), nothing reached
  the internet, including the allowlisted address; that is the host firewall, not the backend.
- Independent of `FORWARD`: the guest reached the host's own sshd on the bridge gateway (10.88.77.1:22)
  and on the host LAN address (192.168.1.7:22), reading the SSH banner. Traffic to a host-local address
  takes the INPUT path, which no rule of the backend's covers.

Consequence: `NetPolicy::allowlist` and `ResourceLimits::net_bytes` (ADR-093, ADR-095) do not do what
their ADRs claim for a Kata guest. The only thing standing between the guest and the network is whatever
the host's firewall happens to say. This touches I2 (no ambient authority) and is the egress path, so it
needs the full design -> red-team -> prove -> judge loop, not a patch. Candidate directions (not
evaluated): enforce on the `forward` and `input` hooks (or at the veth/tc boundary) keyed on the
guest's address; or place the policy where microVM traffic is routed rather than bridged.

## Finding 3 -- Kata 4.1.0 specifics the runbook does not mention

- The 4.1.0 static tarball ships the Rust runtime (`runtime-rs`), not the Go one. The shim is
  `/opt/kata/runtime-rs/bin/containerd-shim-kata-v2`; `io.containerd.kata-clh.v2` needs a wrapper named
  `containerd-shim-kata-clh-v2` on containerd's PATH.
- The shim refuses a custom `KATA_CONF_FILE` ("only shipped Kata configuration files are accepted"); the
  cloud-hypervisor config must be installed as `/etc/kata-containers/runtime-rs/configuration.toml`, which
  makes clh the only runtime that shim can serve.
- The shim creates a `cnitest-<uuid>` network namespace per `ctr run` and does not delete it; they
  accumulate (35 after one afternoon). Not the backend's `ae-kata-*` namespaces.
- A failed first `ctr run` leaves a stale socket in `/run/containerd/s/` ("Address in use").
- The CNI config for a plugin chain must be named `*.conflist`.

## Finding 4 -- environmental assumptions in other tests

- `test_containerd_isolation` L2/L5 hardcoded `eth0`; a host with predictable interface names (`enp7s0`)
  failed both positive controls while the negative checks passed vacuously. Fixed to "any interface other
  than `lo`".
- `test_kata_backend_{slice2,abuse_corpus}_linux` ignored `std::system` results; gcc's `warn_unused_result`
  failed `-Werror`. Replaced with `std::filesystem` calls.
- The containerd tests need root or socket access and a pulled `alpine:latest`.

## What was and was not verified

Verified here: the four suites compile with gcc 15 (Release, `-Werror`); the three passing suites pass;
guest boots and has an address; the allowlist's nft ruleset is what it is (read directly); the
guest-to-host and guest-to-internet reachability above (read directly with `nc` inside the guest).

Not verified: clang for the Kata targets; the `net_bytes` quota in isolation (its check was vacuous and
its enforcement point is the same `output`-hook table); whether the same gap exists on `docker0`-style
setups; any change to the backend (none made).
