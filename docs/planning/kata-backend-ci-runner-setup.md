# KataBackend CI self-hosted runner — provisioning runbook

**Purpose**: what the "real server" the project owner is preparing separately needs, before
`.github/workflows/kata-backend-ci.yml` can run anything beyond its own "Verify runner
preconditions" failure. Not itself automated — each step below is a real, infrastructure-shaped,
root-level operation this project deliberately does not run unattended from a CI job (that workflow's
own header comment states the same reasoning). Compiled 2026-08-24; re-check the cited pages before
following them verbatim if this runbook is used much later — every prior Kata ADR in this repo
(ADR-084 through ADR-095) already discloses that no session so far has run any of this against a real
host, so nothing here has been execution-verified yet either.

## 1. Host requirements

- Linux x86_64, KVM-capable (`/dev/kvm` present — `kvm-ok` or `ls -l /dev/kvm` confirms it), matching
  `kata_backend.hpp`'s own `platform_id::linux_x86_64`-only `ProfileTraits`.
- A distro with systemd (the containerd/Kata install docs below assume it).
- Enough disk for image pulls + Kata's own guest kernel/initrd artifacts — a few GB is typically
  enough for `busybox`-scale test images; size up if the test image ever changes.

## 2. containerd

Install per containerd's own current docs — **do not hand-copy commands from memory**, versions and
release-asset layouts drift:
<https://github.com/containerd/containerd/blob/main/docs/getting-started.md>

Verify: `sudo systemctl status containerd` is active, `ctr version` runs.

## 3. Kata Containers 4.1.0, `kata-clh` runtime class ONLY

This project pinned Kata Containers **4.1.0**, and **only** the `kata-clh` (cloud-hypervisor) runtime
class — never `kata-qemu` — per `decisions/ADR-084-kata-backend-slice-1.md`'s own narrowed scope (a
deliberate CVE-surface-bounding choice, not an oversight). Do not install a different Kata version or
enable `kata-qemu` without a new ADR revisiting that scope.

Official install entry point (current, general): <https://kata-containers.github.io/kata-containers/installation/>
The specific 4.1.0 release, its own release notes, and its downloadable static-tarball assets:
<https://github.com/kata-containers/kata-containers/releases/tag/4.1.0>

Follow the "manual installation with containerd" path from the official docs — install prefix
`/opt/kata/`, binary `containerd-shim-kata-v2` — and add **only** the `kata-clh` runtime to
containerd's `config.toml` as `io.containerd.kata-clh.v2` (the exact runtime type string
`KataBackend`'s own default constructor argument, `src/backends/kata/kata_backend.hpp`, expects).

Verify (the exact same probe `kata-backend-ci.yml`'s "Verify runner preconditions" step runs):
```
sudo ctr run --rm --runtime io.containerd.kata-clh.v2 docker.io/library/busybox:latest precheck true
```

## 4. CNI: `cnitool`, a plugin directory, and a network config named `ae-kata-net`

Needed only for `NetPolicy::allowlist`-exercising tests (Slice 10/`test_kata_backend_slice9_10_linux`
cases 6/9) — `deny_all` tests don't touch this at all.

- `cnitool`: <https://github.com/containernetworking/cni/tree/main/cnitool> (a real, separately-built
  reference CLI, NOT bundled with containerd/`ctr` — `kata_backend.hpp`'s own header comment already
  names this as a new deployment precondition since Slice 10).
- CNI plugin binaries: <https://github.com/containernetworking/plugins/releases> — install to a
  directory (commonly `/opt/cni/bin`), matching `KataBackend`'s own constructor default
  `cni_plugin_dir = "/opt/cni/bin"`.
- A CNI network config file under `/etc/cni/net.d` (matching the constructor default
  `cni_conf_dir = "/etc/cni/net.d"`) whose `"name"` field is **exactly** `ae-kata-net` (the
  constructor default `cni_network_name`) — a bridge-type config is the ordinary choice for this kind
  of single-host CI setup; see the CNI plugins repo's own examples for the JSON shape.

If the runner's constructor arguments ever change from these defaults (a real `KataBackend backend(...)`
call site with non-default `cni_network_name`/`cni_plugin_dir`/`cni_conf_dir`), update this section and
the workflow's own precondition step to match — don't let them silently drift apart.

## 5. `nft` with named-quota support

`ResourceLimits::net_bytes` (ADR-095, Slice 11) needs `nft_quota` kernel/nftables-build support, beyond
the plain `table`/`chain`/`rule ... accept` usage Slice 10 already needed. Most current distro kernels
ship this by default — confirm with a throwaway probe rather than assuming:
```
sudo nft add table inet ae_precheck
sudo nft add quota inet ae_precheck ae_precheck_q { over 1 bytes }
sudo nft delete table inet ae_precheck
```
If the `add quota` line errors, the kernel/nftables build lacks `nft_quota` support — this needs a
kernel/nftables upgrade, not a code change.

## 6. `losetup` / `mkfs.ext4` / loop-device kernel support

`ResourceLimits::disk_bytes` (ADR-095, Slice 11) needs a loop-device-capable kernel (`CONFIG_BLK_DEV_LOOP`,
on by default on essentially every distro kernel) and `e2fsprogs` (`mkfs.ext4`) — both installed by
default on most server distros; confirm with `command -v losetup mkfs.ext4`.

## 7. Passwordless sudo for the runner's service account

Every host-side call `kata_backend.cpp` makes (`ctr`, `ip netns`, `cnitool`, `nft`, `losetup`, `mount`,
`umount`, `mkfs.ext4`, `fallocate`) needs root. The GitHub Actions self-hosted runner process itself
should run as an unprivileged service account (standard self-hosted-runner security guidance —
<https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/security-hardening-for-self-hosted-runners>),
with passwordless `sudo` scoped to that one account so the workflow's own `sudo ctr ...`/`sudo ctest ...`
steps work without an interactive prompt (which would hang the job — `Bash`'s own guidance elsewhere in
this project about non-interactive shells applies equally to a CI runner).

## 8. Register the self-hosted runner

GitHub's own current docs (this step is genuinely just "follow GitHub's instructions," nothing
project-specific to add): <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners>

Register with labels matching `.github/workflows/kata-backend-ci.yml`'s `runs-on: [self-hosted, linux,
kata]` — or edit that workflow's `runs-on` line to match whatever labels are actually used; the two
must agree or the job queues forever with no runner able to pick it up.

## 9. First run

Trigger `kata-backend-ci.yml` manually (Actions tab → "KataBackend (self-hosted, live deployment)" →
"Run workflow", `workflow_dispatch` only, deliberately not on every push — see that workflow's own
header comment). Its own "Verify runner preconditions" step re-checks everything above and fails with
a specific, actionable error naming exactly what's still missing, rather than a confusing test-suite
failure — trust that diagnostic over re-reading this whole runbook top to bottom on a re-run.

## Open items this runbook does not resolve

- Exact CNI bridge-config JSON is not written out here — the CNI plugins repo's own examples are the
  authoritative source, and a specific config depends on the host's own network topology (which this
  runbook cannot know in advance).
- Nothing here automates image pre-pulling (`docker.io/library/busybox:latest`, the test suite's
  default `KataBackend` image) — the first live run will pull it, adding to that run's own wall-clock
  time; a future iteration could pre-pull it during provisioning instead.
- No guidance on updating Kata Containers past 4.1.0 — that is a scope decision (`decisions/ADR-084-...md`'s
  own pin), not a provisioning detail; revisit via a new ADR, not by silently bumping the version here.
