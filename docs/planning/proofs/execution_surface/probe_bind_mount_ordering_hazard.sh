#!/bin/bash
# PROVE-PHASE PROBE (OCI ExecutionSurface design, docs/planning/oci-execution-surface-design-draft.md
# §4 finding 1 / §5's own "most important open item"): a real, empirical test of the ONE unresolved
# risk that design-only red-team round found and could not prove by reasoning alone --
# SandboxRuntime::run() (execution_surface/sandbox_runtime.hpp) calls materialize() (a full
# remove_all()+create_directories() of the staging directory, real_io_filesystem.hpp:299-301)
# BEFORE reset(), unconditionally, on every turn. For a bind-mount ExecutionSurface conformer, the
# PREVIOUS turn's container may still be alive and still bind-mounted at that exact host path one
# step before reset() would destroy it. This probe reproduces exactly that sequence against a REAL
# containerd+runc deployment (Ubuntu 26.04 WSL2, containerd 2.2.2, runc 1.4.0 -- apt-installed,
# no live deployment existed anywhere reachable before this probe) and records the real answer.
#
# Run as root (containerd's default socket permissions on this install require it). Idempotent:
# cleans up any leftover container/snapshot state from a prior run before starting.
#
# REAL RESULT, first clean run (2026-08-27, quoted verbatim in the design doc, not paraphrased):
#   - Host-side `rm -rf hostdir; mkdir hostdir` (exactly materialize()'s own operation) succeeds
#     cleanly (rc=0 both) while the OLD container is STILL RUNNING and still bind-mounted -- no
#     error, no hang.
#   - The OLD, still-running container survives (`ctr tasks ls` shows it RUNNING throughout).
#   - The OLD container's bind-mounted view of the recreated path becomes a genuinely EMPTY
#     directory (`total 0`) -- NOT stale-but-intact (it does not still show turn 1's content), and
#     NOT leaking turn 2's fresh content either (host and old-container views are cleanly
#     disconnected the moment the host recreates the directory).
#   - Destroying the old container (`ctr task kill`/`ctr task rm`/`ctr container rm` -- exactly
#     reset()'s own first action) succeeds cleanly.
#   - A FRESH container, bind-mounted at the SAME (now-recreated) host path, sees ONLY the fresh
#     turn-2 content -- no leakage of turn-1 content, no corruption.
#   - A write from the fresh container lands correctly on the real host disk, confirming the
#     write-through round trip still works after a materialize()/reset() cycle.
#
# CONCLUSION: the design's own reasoning (§4 finding 1: "plausibly benign... but this is reasoning,
# not a real test") is now a real, empirical result, not an assumption. The bind-mount architecture
# is safe against this specific ordering hazard, for THIS containerd/runc/kernel combination -- named
# precisely, not overclaimed as universally proven for every possible filesystem backend a real
# deployment might use for its staging directory (this probe used WSL2's own ext4-backed /tmp).

set -e

CTR_ID_A="ae_ordering_probe_a"
CTR_ID_B="ae_ordering_probe_b"
HOST_DIR="/tmp/ae_ordering_probe/hostdir"
IMAGE="docker.io/library/alpine:latest"

cleanup() {
    for id in "$CTR_ID_A" "$CTR_ID_B"; do
        ctr task kill --signal SIGKILL "$id" >/dev/null 2>&1 || true
        sleep 0.5
        ctr task rm "$id" >/dev/null 2>&1 || true
        ctr container rm "$id" >/dev/null 2>&1 || true
        ctr snapshot rm "$id" >/dev/null 2>&1 || true
    done
    rm -rf "$(dirname "$HOST_DIR")"
}

systemctl start containerd >/dev/null 2>&1 || true
sleep 1
cleanup  # idempotent: clean any leftover state from a prior run before starting

mkdir -p "$HOST_DIR"
echo "turn1-content" > "$HOST_DIR/a.txt"
ctr images pull "$IMAGE" >/dev/null 2>&1

echo "=== [1] start $CTR_ID_A, bind-mounted at $HOST_DIR ==="
ctr run -d --mount type=bind,src="$HOST_DIR",dst=/workspace,options=rbind:rw "$IMAGE" "$CTR_ID_A" sleep infinity
sleep 1

echo "=== [2] container sees turn-1 content via the live bind mount ==="
ctr tasks exec --exec-id e1 "$CTR_ID_A" /bin/sh -c "cat /workspace/a.txt"

echo "=== [3] THE HAZARD: host-side remove_all+recreate (materialize()'s own operation), while ==="
echo "===     $CTR_ID_A is STILL RUNNING and still bind-mounted at this exact path ==="
rm -rf "$HOST_DIR"; echo "rm -rf exit code: $?"
mkdir -p "$HOST_DIR"; echo "mkdir exit code: $?"
echo "turn2-fresh-content" > "$HOST_DIR/b.txt"

echo "=== [4] is the old container still alive/healthy? ==="
ctr tasks ls

echo "=== [5] old container's view of /workspace now (expect: orphaned, empty -- not stale, not corrupted) ==="
ctr tasks exec --exec-id e2 "$CTR_ID_A" /bin/sh -c "ls -la /workspace"

echo "=== [6] destroy the old container -- exactly reset()'s own first action ==="
ctr task kill --signal SIGKILL "$CTR_ID_A"
sleep 1
ctr task rm "$CTR_ID_A"
ctr container rm "$CTR_ID_A"
ctr snapshot rm "$CTR_ID_A" >/dev/null 2>&1 || true
echo "old container destroyed cleanly"

echo "=== [7] start a FRESH container bind-mounted at the SAME, now-recreated host path ==="
ctr run -d --mount type=bind,src="$HOST_DIR",dst=/workspace,options=rbind:rw "$IMAGE" "$CTR_ID_B" sleep infinity
sleep 1

echo "=== [8] fresh container's view -- should see ONLY turn-2 content, no leakage from turn 1 ==="
ctr tasks exec --exec-id e3 "$CTR_ID_B" /bin/sh -c "ls -la /workspace; cat /workspace/b.txt"

echo "=== [9] a write from the fresh container round-trips onto the real host disk ==="
ctr tasks exec --exec-id e4 "$CTR_ID_B" /bin/sh -c "echo written-from-container > /workspace/c.txt"
cat "$HOST_DIR/c.txt"

echo "=== [10] cleanup ==="
cleanup
echo "ALL STEPS COMPLETE -- see this file's own header comment for the real, recorded result."
