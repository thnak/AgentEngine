#!/usr/bin/env bash
# TEST-ONLY environment bootstrap for test_native_jail_backend_linux, run inside a --privileged
# Docker container before the test binary. Stands in for what a real deployment's one-time host
# init would do: cgroups v2 requires memory/pids to be enabled in cgroup.subtree_control at EVERY
# ancestor of the leaf cgroup a process is actually placed in, and a cgroup cannot have its own
# subtree_control written while it still has processes directly resident in it (the kernel's "no
# internal processes" rule) -- which is exactly the state a fresh container's root cgroup starts
# in (the container's own shell/init process lives there). This script moves those processes into
# a sibling cgroup first, then enables the controllers, matching linux_native_jail_backend.hpp's
# own stated precondition rather than having product code perform a host-wide process-cgroup move
# as a side effect of a per-sandbox create() call.
#
# Usage: `source cgroup_v2_test_setup.sh && <run the test binary>` -- SOURCED, not executed as a
# subprocess: this script moves every process resident in the root cgroup (including its own
# caller) into a sibling cgroup before enabling controllers. Running it via `bash script.sh`
# introduces the invoking shell's own new child process (the script interpreter itself) as an
# additional root-cgroup resident that the move-then-enable dance then has to additionally chase --
# observed directly in this harness. Sourcing runs every command in the CALLING shell, so there is
# only ever the one process to move, matching this script's own logic.
set -euo pipefail

ROOT=/sys/fs/cgroup
INIT_CGROUP="$ROOT/init"

mkdir -p "$INIT_CGROUP"

# Retried, not one-shot: moving every currently-listed pid out of $target is a snapshot-then-act
# operation, and a process legitimately still finishing (e.g. a build subprocess exiting a moment
# after this script starts) can transiently re-populate the source cgroup between the snapshot and
# the subtree_control write, producing an EBUSY that clears itself within a few hundred ms. Real,
# observed behavior in this Docker-based harness, not a hypothetical -- retried rather than treated
# as fatal on the first EBUSY.
enable_controllers() {
  local target="$1"
  local attempt
  for attempt in 1 2 3 4 5; do
    # A bulk read (command substitution, like `cat`), NOT `while read -r ... < file`: cgroup.procs
    # is a kernel seq_file, and bash's `read` builtin does small/byte-at-a-time reads looking for
    # each line's newline -- observed directly in this harness to silently return only the FIRST
    # entry before behaving as if EOF, every remaining resident process never even attempted. A
    # single bulk read (one or few large read() syscalls, exactly what `cat`/command substitution
    # do) gets the real, complete listing.
    for pid in $(cat "$target/cgroup.procs" 2>/dev/null); do
      echo "$pid" > "$INIT_CGROUP/cgroup.procs" 2>/dev/null || true
    done
    if echo "+memory +pids" > "$target/cgroup.subtree_control" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done
  echo "cgroup v2 test setup: FAILED to enable controllers on $target after 5 attempts" >&2
  for pid in $(cat "$target/cgroup.procs" 2>/dev/null); do
    echo "resident pid=$pid comm=$(cat /proc/$pid/comm 2>/dev/null) cmdline=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null)" >&2
  done
  return 1
}

enable_controllers "$ROOT"

mkdir -p "$ROOT/agentengine"
enable_controllers "$ROOT/agentengine"

echo "cgroup v2 test setup: OK"
