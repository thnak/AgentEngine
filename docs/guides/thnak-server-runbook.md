# thnak-server runbook (build box + Kata test host)

State as audited 2026-10-20. Facts here come from that audit and from
`docs/planning/2026-10-19-*` (on branch `kata-live-verification`); re-check before relying on them.

## 1. What the machine is

| | |
|---|---|
| Access | `ssh thnak-server`, user `thnak`, passwordless sudo (`/etc/sudoers.d/thnak-nopasswd`) |
| Hardware | Xeon E5-2680 v4, 28 cores, 61 GB RAM, 232 GB disk (9% used), VT-x, `/dev/kvm` present |
| OS | Ubuntu 26.04, kernel 7.0.0-38 |
| Network | `enp7s0` 192.168.1.7/24. Listens only on SSH (22, with fail2ban) and local DNS |
| Role | Dedicated build box: use every core (`-j$(nproc)`); this overrides the repo's `-j4` for this host only |
| Toolchain | gcc 15, clang 21, cmake 4.2, ninja, ccache (20 GB cap), mold. Never build in Docker (slow) |
| Services | containerd 2.4.2, Docker, fail2ban, sshd |

## 2. Building

Run from Git Bash in the repo: `bash tools/ssh-linux-build.sh [--clang] [--test] [ninja targets...]`.
It ships `git ls-files` (including uncommitted edits) to `~/ae/src` and builds in `~/ae/build-gcc` or
`~/ae/build-clang`. Build with both compilers before pushing; CI's clang-cl and gcc legs fail on
`-Werror` diagnostics MSVC never reports. Cold build: gcc about 5 min, clang about 8 min.

Kata-enabled build dir (`~/ae/build-kata`, gcc):

```
cmake -S ~/ae/src -B ~/ae/build-kata -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAGENTENGINE_BUILD_KATA_BACKEND=ON -DAGENTENGINE_KATA_SANDBOX_TESTS=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_LINKER_TYPE=MOLD
```

Other build dirs there (`build-clang-kata`, `build-gcc-kata`) are older and can be deleted to save
about 1.5 GB if the disk gets tight.

## 3. Kata setup

- Kata 4.1.0, the **Rust runtime-rs** (not the Go runtime), in `/opt/kata`.
- Shim: `/usr/local/bin/containerd-shim-kata-clh-v2` -> `/opt/kata/runtime-rs/bin/containerd-shim-kata-v2`.
  Runtime class `io.containerd.kata-clh.v2` (Cloud Hypervisor is the only VMM usable through this shim).
- The shim rejects a custom `KATA_CONF_FILE`, so the config is the copy at
  `/etc/kata-containers/runtime-rs/configuration.toml`.
- CNI: plugins v1.9.1 in `/opt/cni/bin`, `cnitool` in `/usr/local/bin`, network
  `/etc/cni/net.d/10-ae-kata-net.conflist` (name `ae-kata-net`, bridge `aekata0`, 10.88.77.0/24).
- Images pulled into containerd (`default` namespace): `busybox`, `alpine:latest`, `python:3.12-alpine`.
- Smoke test: `sudo ctr run --rm --runtime io.containerd.kata-clh.v2 docker.io/library/busybox:latest t uname -a`
  (boots a guest kernel in about 6 s).

## 4. Running the Kata suites

`FORWARD` is the host firewall chain that decides traffic passing *through* the host. Docker and ufw leave
its policy at `drop`, so microVM traffic to the internet is blocked unless you add accept rules. The
networking suites (`slice9_10`, `egress_*`) need those rules; the others do not.

No rules needed: `test_kata_egress_policy_render`, `test_kata_backend_linux`,
`test_kata_backend_slice2_linux`, `test_kata_backend_abuse_corpus_linux`.

Rules needed (tests check for the comment `ae-prove-temp`): `test_kata_backend_slice9_10_linux`,
`test_kata_backend_egress_linux` (about 3 min), `test_kata_backend_egress_lifecycle_linux` (about 2 min),
`test_kata_egress_scale_linux`.

Always add the rules with a trap so they are removed even if a run dies:

```
cleanup() { while sudo iptables -S FORWARD | grep -q ae-prove-temp; do
  r=$(sudo iptables -S FORWARD | grep ae-prove-temp | head -1 | sed 's/^-A/-D/'); eval "sudo iptables $r"; done; }
trap cleanup EXIT
for i in aekata0 aepvw0; do
  sudo iptables -I FORWARD -i $i -m comment --comment ae-prove-temp -j ACCEPT
  sudo iptables -I FORWARD -o $i -m comment --comment ae-prove-temp -j ACCEPT
done
cd ~/ae/build-kata && sudo ctest -R "slice9_10|egress_linux|egress_lifecycle" --timeout 1600 --output-on-failure
```

Last result (2026-10-20, branch `adr241-followups`): all seven suites above pass.
While the rules are in place microVMs on `aekata0` can reach the LAN and internet, so keep the window short.

## 5. Gotchas

- **Never `pkill -f <name>` inside an ssh command string**: it matches and kills the ssh shell itself.
- A failed first `ctr run` leaves a stale socket in `/run/containerd/s/` ("Address in use"); delete it.
- Prefer plain `ssh` from Git Bash over background MCP ssh tasks (late, noisy completion notices).
- `test_json_dump_escape` fails here because the kernel does not enforce the memory cap. Expected.
- The backend's own nft allowlist did **not** filter microVM traffic in the 2026-10-19 findings; with default
  `FORWARD drop` the host firewall was what blocked the guest. ADR-240 / issue #194 is the fix. Do not
  treat a passing "guest cannot reach X" check as proof of the allowlist unless a positive control passes too.

## 6. Cleanup and audit checklist

Run read-only after any Kata session:

```
sudo iptables -S FORWARD | grep -c ae-prove-temp     # want 0
ls /run/netns | wc -l                                # want 0 (leftovers are named cnitest-*)
ip -br link | grep -E 'aepvw|veth'                   # want nothing (aekata0 itself stays)
sudo ctr -n default tasks ls                         # want only the header
grep -n ae- /etc/hosts                               # want nothing
ls /tmp | grep -E 'aepv|ae-'                         # aepv-sink.py / aepv-sniff.py / ae-design are known
```

Stale `cnitest-*` namespaces (each reports "Peer netns reference is invalid") accumulate from interrupted
CNI tests; remove with `for n in $(ls /run/netns | grep '^cnitest-'); do sudo ip netns delete $n; done`.
On 2026-10-20 about 70 were removed this way.

## 7. Capacity (measured, idle busybox guests, default 1 vCPU / 2 GiB)

Cold boot 5.8 s. 40 VMs boot in parallel in 6.5 s. Warm `exec` p50 72 ms. About 170 MB of host RAM per idle VM
(40 VMs = 6.7 GB), but a busy guest may use up to 2 GiB, so about 28 busy VMs fit by RAM. Not tested: guests
under load, more than 40 real VMs, 200 real microVMs.
