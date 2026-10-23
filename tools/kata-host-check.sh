#!/usr/bin/env bash
# Read-only check of whether a machine can run the KataBackend (src/backends/kata/) and its live test suites,
# against the preconditions kata_backend.hpp and ADR-240 (Kata egress policy) state. It changes nothing: no
# modprobe, no image pull, no firewall or bridge edit, so it is safe on a shared host.
#
# Run from Git Bash or a Linux shell:
#   bash tools/kata-host-check.sh                      # this machine (Linux, or Windows to see why not)
#   bash tools/kata-host-check.sh --host root@1.2.3.4  # a server over SSH (the script is piped, not copied)
#
# Exit 0 when nothing FAILs (WARN items are allowed), 1 otherwise. Sections:
#   platform, virtualization, containerd + Kata, CNI, egress policy (ADR-240), test prerequisites, host hygiene.
# Needs root (or passwordless sudo) for the nft/iptables/ctr reads; without it those items WARN as unreadable.
# No pipefail: several checks are `producer | grep -q`, and a producer that exits non-zero (`bridge link help`)
# or takes SIGPIPE from grep's early exit would turn a match into a false FAIL.
set -u

if [ "${1:-}" = "--host" ]; then
    [ -n "${2:-}" ] || { echo "usage: $0 [--host user@host]" >&2; exit 2; }
    exec ssh -o BatchMode=yes "$2" 'bash -s' < "$0"
fi

n_ok=0; n_warn=0; n_fail=0
ok()   { n_ok=$((n_ok + 1));     printf '[ OK ] %s\n' "$*"; }
warn() { n_warn=$((n_warn + 1)); printf '[WARN] %s\n' "$*"; }
fail() { n_fail=$((n_fail + 1)); printf '[FAIL] %s\n' "$*"; }
hint() { printf '       -> %s\n' "$*"; }
section() { printf '\n== %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
# ver_ge A B: true when version A >= B
ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }

# ---- Windows / macOS: Kata is Linux-only ---------------------------------------------------------------------
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        section "platform: Windows"
        fail "KataBackend is Linux-only (CMake refuses AGENTENGINE_BUILD_KATA_BACKEND on WIN32; Kata needs Linux KVM)"
        hint "use a Linux host: bare metal, or a VM with nested virtualization; check it with --host"
        if have powershell.exe; then
            fw="$(powershell.exe -NoProfile -Command '(Get-CimInstance Win32_Processor | Select-Object -First 1).VirtualizationFirmwareEnabled' 2>/dev/null | tr -d '\r')"
            [ "$fw" = "True" ] && ok "CPU virtualization is enabled in firmware (a local Linux VM could host Kata with nested virt)" \
                               || warn "CPU virtualization in firmware: ${fw:-unknown} (a local Linux VM would need it, plus nested virt)"
            hv="$(powershell.exe -NoProfile -Command '(Get-CimInstance Win32_ComputerSystem).HypervisorPresent' 2>/dev/null | tr -d '\r')"
            printf '       hypervisor present: %s\n' "${hv:-unknown}"
        fi
        if have wsl.exe; then
            printf '       WSL: %s\n' "$(wsl.exe --status 2>/dev/null | tr -d '\r\000' | grep -m1 -i 'version' || echo 'installed (status unreadable)')"
            hint "WSL2 is not a tested Kata host for this project, and the owner's rule is no WSL on the dev machine"
        fi
        printf '\nSummary: %d ok, %d warn, %d fail -- Kata: NOT SUPPORTED on this OS\n' "$n_ok" "$n_warn" "$n_fail"
        exit 1 ;;
    Darwin)
        echo "[FAIL] KataBackend is Linux-only; macOS cannot host it"; exit 1 ;;
esac

# ---- privilege -------------------------------------------------------------------------------------------------
if [ "$(id -u)" = 0 ]; then SUDO=""; priv=root
elif sudo -n true 2>/dev/null; then SUDO="sudo -n"; priv=sudo
else SUDO=""; priv=none; fi
as_root() { if [ "$priv" = none ]; then return 126; fi; $SUDO "$@"; }

# ---- platform --------------------------------------------------------------------------------------------------
section "platform"
. /etc/os-release 2>/dev/null || true
printf '       host %s, %s, kernel %s, %s\n' "$(hostname)" "${PRETTY_NAME:-unknown OS}" "$(uname -r)" "$(uname -m)"
case "$(uname -m)" in
    x86_64|aarch64) ok "architecture $(uname -m) has Kata static releases" ;;
    *) fail "architecture $(uname -m) has no Kata static release" ;;
esac
kver="$(uname -r | sed 's/[^0-9.].*//')"
if ver_ge "$kver" 6.3; then ok "kernel $kver >= 6.3 (ADR-240 egress floor)"
else fail "kernel $kver < 6.3: the egress policy's bridge port flags / nft bridge features are not available"; fi
case "$priv" in
    root) ok "running as root" ;;
    sudo) ok "passwordless sudo available" ;;
    none) warn "not root and no passwordless sudo: the Kata tests need root, and root-only checks below are skipped" ;;
esac
cores="$(nproc)"; mem_gb="$(awk '/MemTotal/ {printf "%d", $2/1048576}' /proc/meminfo)"
avail_gb="$(awk '/MemAvailable/ {printf "%d", $2/1048576}' /proc/meminfo)"
printf '       %s cores, %s GiB RAM (%s GiB available)\n' "$cores" "$mem_gb" "$avail_gb"
if [ "$avail_gb" -ge 8 ]; then ok "available memory ${avail_gb} GiB (an idle guest is ~170 MB, a busy one up to 2 GiB)"
else warn "only ${avail_gb} GiB available: the suites boot several 2 GiB microVMs"; fi
disk_gb="$(df -BG --output=avail /var/lib 2>/dev/null | tail -1 | tr -dc 0-9)"
if [ "${disk_gb:-0}" -ge 20 ]; then ok "${disk_gb} GiB free under /var/lib (containerd images + snapshots)"
else warn "${disk_gb:-?} GiB free under /var/lib; images, snapshots and loop-mounted disks need room"; fi

# ---- virtualization --------------------------------------------------------------------------------------------
section "virtualization"
# On bare metal systemd-detect-virt prints "none" AND exits non-zero, so `|| echo none` would append a second "none".
virt="$(systemd-detect-virt 2>/dev/null || true)"; virt="${virt:-none}"
if [ "$virt" = none ]; then ok "bare metal"
else warn "running inside a '$virt' VM: Kata needs nested virtualization (checked below via the CPU flag and /dev/kvm)"; fi
if grep -q -m1 -E '\b(vmx|svm)\b' /proc/cpuinfo; then ok "CPU exposes hardware virtualization (vmx/svm)"
else fail "no vmx/svm CPU flag: KVM is unavailable (enable VT-x/AMD-V, or nested virt on the hypervisor)"; fi
if [ -c /dev/kvm ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ok "/dev/kvm present and usable by $(id -un)"
    elif as_root sh -c 'test -r /dev/kvm && test -w /dev/kvm' 2>/dev/null; then
        ok "/dev/kvm present and usable by root (the Kata tests run as root; $(id -un) is not in the kvm group)"
    else warn "/dev/kvm present but not read/writable by $(id -un), and not checkable as root (fine if the tests run as root)"; fi
else fail "/dev/kvm missing"; hint "load kvm_intel/kvm_amd; inside a VM enable nested virtualization on the hypervisor"; fi
for m in kvm_intel kvm_amd; do
    [ -r "/sys/module/$m/parameters/nested" ] && printf '       %s nested=%s\n' "$m" "$(cat /sys/module/$m/parameters/nested)"
done
if [ -c /dev/vhost-vsock ]; then ok "/dev/vhost-vsock present (Kata agent channel)"
elif modinfo vhost_vsock >/dev/null 2>&1; then warn "/dev/vhost-vsock absent but module vhost_vsock exists (Kata loads it; 'modprobe vhost_vsock' to confirm)"
else fail "no vhost-vsock device or module: the Kata agent cannot talk to the guest"; fi
if [ -c /dev/vhost-net ] || modinfo vhost_net >/dev/null 2>&1; then ok "vhost-net available"
else warn "vhost-net unavailable (slower guest networking)"; fi

# ---- containerd + Kata -----------------------------------------------------------------------------------------
section "containerd + Kata"
if have containerd; then
    cv="$(containerd --version 2>/dev/null | awk '{print $3}' | sed 's/^v//')"
    ok "containerd $cv"
    if systemctl is-active --quiet containerd 2>/dev/null || pgrep -x containerd >/dev/null; then ok "containerd daemon running"
    else fail "containerd is installed but not running"; fi
else fail "containerd not installed"; fi
have ctr && ok "ctr present" || fail "ctr missing (the backend drives containerd through ctr)"
# runtime type io.containerd.kata-clh.v2 resolves to the binary containerd-shim-kata-clh-v2 on PATH
if have containerd-shim-kata-clh-v2; then ok "shim containerd-shim-kata-clh-v2 on PATH (runtime io.containerd.kata-clh.v2)"
else fail "containerd-shim-kata-clh-v2 not on PATH: runtime io.containerd.kata-clh.v2 cannot start"
     hint "install the Kata static tarball to /opt/kata and add a wrapper/symlink in /usr/local/bin"; fi
if [ -d /opt/kata ]; then
    kv="$(cat /opt/kata/VERSION 2>/dev/null || /opt/kata/bin/containerd-shim-kata-v2 --version 2>/dev/null | head -1)"
    ok "/opt/kata present${kv:+ ($kv)}"
    # The Kata tarball installs cloud-hypervisor mode 744 (root only), so test and run it as root: `-x` as an
    # unprivileged user would report an installed binary as missing.
    if as_root test -x /opt/kata/bin/cloud-hypervisor; then
        ok "cloud-hypervisor present ($(as_root /opt/kata/bin/cloud-hypervisor --version 2>/dev/null | head -1))"
    elif [ -e /opt/kata/bin/cloud-hypervisor ]; then
        warn "/opt/kata/bin/cloud-hypervisor exists but cannot be checked without root or passwordless sudo"
    else fail "/opt/kata/bin/cloud-hypervisor missing (the backend pins kata-clh)"; fi
else fail "/opt/kata missing (Kata Containers not installed)"; fi
if [ -f /etc/kata-containers/runtime-rs/configuration.toml ]; then ok "/etc/kata-containers/runtime-rs/configuration.toml present"
elif [ -f /etc/kata-containers/configuration.toml ]; then warn "only the Go-runtime config path exists; Kata 4.x runtime-rs reads /etc/kata-containers/runtime-rs/configuration.toml"
else warn "no /etc/kata-containers config: the shim falls back to /opt/kata defaults (and refuses a custom KATA_CONF_FILE)"; fi

# ---- CNI -------------------------------------------------------------------------------------------------------
section "CNI (network ae-kata-net)"
have cnitool && ok "cnitool present" || fail "cnitool missing (github.com/containernetworking/cni/cnitool; not bundled with containerd)"
conf="$(grep -l '"ae-kata-net"' /etc/cni/net.d/*.conflist 2>/dev/null | head -1)"
bridge=""
if [ -n "$conf" ]; then
    ok "network ae-kata-net defined in $conf"
    bridge="$(grep -o '"bridge"[[:space:]]*:[[:space:]]*"[^"]*"' "$conf" | head -1 | sed 's/.*"\([^"]*\)"$/\1/')"
    for t in $(grep -o '"type"[[:space:]]*:[[:space:]]*"[^"]*"' "$conf" | sed 's/.*"\([^"]*\)"$/\1/' | sort -u); do
        [ -x "/opt/cni/bin/$t" ] && ok "CNI plugin $t in /opt/cni/bin" || fail "CNI plugin $t (used by $conf) missing from /opt/cni/bin"
    done
else
    fail "no /etc/cni/net.d/*.conflist names the network ae-kata-net (the extension must be .conflist)"
    hint "a bridge network on a DEDICATED bridge (aekata0, 10.88.77.0/24 on the reference host)"
fi
if [ -n "$bridge" ]; then
    if [ -d "/sys/class/net/$bridge/bridge" ]; then
        foreign=""
        for p in $(ls "/sys/class/net/$bridge/brif" 2>/dev/null); do
            ip -d link show "$p" 2>/dev/null | grep -q ' veth ' || foreign="$foreign $p"
        done
        if [ -z "$foreign" ]; then ok "bridge $bridge exists and has only veth ports"
        else fail "bridge $bridge has non-veth ports:$foreign -- ADR-240 needs a bridge dedicated to Kata (unregistered ports are default-dropped)"; fi
    else
        ok "bridge $bridge not created yet (the CNI bridge plugin creates it on first use)"
    fi
    case "$bridge" in docker0|br-*|virbr*|lxdbr*|cni0) fail "bridge name $bridge belongs to another stack; ADR-240's policy would drop its traffic" ;; esac
fi

# ---- egress policy (ADR-240) -----------------------------------------------------------------------------------
section "egress policy (ADR-240)"
if have nft; then
    nv="$(nft --version 2>/dev/null | awk '{print $2}' | sed 's/^v//')"
    if ver_ge "$nv" 1.0.9; then ok "nft $nv >= 1.0.9 ('destroy' is needed by removal and sweep)"
    else fail "nft $nv < 1.0.9: create would succeed but teardown/sweep fail forever (judgement S3)"; fi
else fail "nft missing"; fi
have tc && ok "tc present" || fail "tc missing (iproute2)"
if bridge link help 2>&1 | grep -q locked; then ok "bridge tool supports the 'locked' port flag"
else fail "the bridge tool does not know 'locked' (iproute2 too old)"; fi
for m in cls_flower sch_ingress nf_tables nf_conntrack_bridge bridge; do
    if [ -d "/sys/module/$m" ]; then ok "module $m loaded"
    elif modinfo "$m" >/dev/null 2>&1; then ok "module $m available (loaded on demand)"
    else fail "module $m neither loaded nor available"; fi
done
if [ -d /sys/module/br_netfilter ]; then warn "br_netfilter is loaded: the ADR-240 policy was never tested with it (bridged frames also hit iptables)"
else ok "br_netfilter not loaded (the tested configuration)"; fi
if fwd="$(as_root iptables -S FORWARD 2>/dev/null | head -1)"; then
    case "$fwd" in
        "-P FORWARD DROP") warn "host FORWARD policy is DROP: guests have no internet egress without an operator accept for the Kata bridge (the E19 / slice9_10 case-6 prerequisite; tests expect comment ae-prove-temp)" ;;
        *) ok "host FORWARD: ${fwd#-P FORWARD }" ;;
    esac
else warn "cannot read iptables FORWARD (needs root)"; fi

# ---- test prerequisites ----------------------------------------------------------------------------------------
section "test prerequisites (tests/backends/kata)"
for t in python3 losetup mkfs.ext4 ip; do have "$t" && ok "$t present" || fail "$t missing"; done
if imgs="$(as_root ctr images ls -q 2>/dev/null)"; then
    for i in docker.io/library/python:3.12-alpine docker.io/library/alpine:latest; do
        printf '%s\n' "$imgs" | grep -qx "$i" && ok "image $i pulled" || warn "image $i not pulled (the suites fail every create without it: ctr images pull $i)"
    done
else warn "cannot list containerd images (needs root)"; fi

# ---- host hygiene and sharing ----------------------------------------------------------------------------------
section "host hygiene"
if have docker && running="$(as_root docker ps -q 2>/dev/null | wc -l)" && [ "$running" -gt 0 ]; then
    warn "$running Docker containers are running: a shared host. The live egress suites load global bridge hooks, turn on bridge conntrack and run flood/scale tests; prefer a dedicated box"
else ok "no running Docker containers"; fi
stale="$(ls -d /run/netns/cnitest-* 2>/dev/null | wc -l)"
[ "$stale" -eq 0 ] && ok "no leaked /run/netns/cnitest-* (Kata shim leak)" || warn "$stale leaked /run/netns/cnitest-* (safe to delete when no experiment runs)"
if tables="$(as_root nft list tables 2>/dev/null)"; then
    printf '%s\n' "$tables" | grep -q 'bridge ae_k' && warn "nft table 'bridge ae_k' exists: sandboxes are live or a crashed backend left one (the next create sweeps it)" \
                                                   || ok "no leftover 'bridge ae_k' table"
fi
if rules="$(as_root iptables -S FORWARD 2>/dev/null)"; then
    tmp="$(printf '%s\n' "$rules" | grep -c 'ae-prove-temp\|ae-judge-temp')"
    [ "$tmp" -eq 0 ] && ok "no temporary ae-*-temp FORWARD accepts left" || warn "$tmp temporary ae-*-temp FORWARD accepts are present"
fi

# ---- verdict ---------------------------------------------------------------------------------------------------
printf '\nSummary: %d ok, %d warn, %d fail -- Kata backend + live suites: %s\n' "$n_ok" "$n_warn" "$n_fail" \
    "$([ "$n_fail" -eq 0 ] && echo READY || echo 'NOT READY')"
[ "$n_fail" -eq 0 ]
