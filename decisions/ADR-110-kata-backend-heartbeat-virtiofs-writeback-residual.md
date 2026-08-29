# ADR-110 — ADR-109's heartbeat/virtiofs residual: a reasoned fix, honestly not live-reverified

- **Status:** Proposed — root cause reasoned from the active Kata configuration and documented FUSE
  writeback semantics, fix applied (2026-08-29), **NOT re-verified against a live deployment**. This
  same pass found a separate, real, blocking environment defect (`ctr images mount` — see §4) that
  prevented every `KataBackend::create()` call in this WSL2 session, including the one this ADR's own
  fix needed to re-run. Named accurately rather than claimed proven.
- **Date:** 2026-08-29.
- **Scope:** `tests/test_kata_backend_abuse_corpus_linux.cpp` only (Case 1 and Case 2's guest-side
  heartbeat-writing loops, plus the file's own top-of-file disclosure comment). No production code
  touched — `src/backends/kata/kata_backend.cpp`/`.hpp` are unchanged by this ADR.
- **Related specs:** `decisions/ADR-109-kata-backend-rootfs-writable-layer-fix.md` §8 (the residual
  this ADR closes the reasoning half of: "the guest process's own heartbeat file stops changing ...
  the measured heartbeat values were both empty (v1= v2=), ... plausibly `virtio_fs_cache = "auto"`'s
  own write-back caching semantics interacting with a forceful guest-side kill, though not confirmed")
  · `/opt/kata/share/defaults/kata-containers/runtime-rs/configuration-clh-runtime-rs.toml` (the live,
  installed Kata clh config this ADR read directly to confirm the active cache mode — not assumed).

## 1. The question

ADR-109 §8 disclosed, but did not diagnose, one remaining `test_kata_backend_abuse_corpus_linux`
failure: Case 1's heartbeat-stopped-changing containment proof observed the heartbeat file **empty**
on both reads (`v1=v2=`, 1 second apart), rather than non-empty-and-unchanged. Is the suspected cause
— `virtio_fs_cache = "auto"`'s write-back caching interacting with the forceful guest-side kill — the
real one, and if so, what closes it without weakening what the test actually proves (that the guest
process, not just the host-side `ctr` CLI call, stopped)?

## 2. Diagnosis

Read directly from the live, installed config this session's Kata deployment actually uses
(`/opt/kata/share/defaults/kata-containers/runtime-rs/configuration-clh-runtime-rs.toml:146`):

```
virtio_fs_cache = "auto"
```

The same file's own doc comment for this setting (lines 137–146): *"auto — Metadata and pathname
lookup cache expires after a configured amount of time (default is 1 second). Data is cached while
the file is open (close to open consistency)."* This is Linux FUSE's writeback-cache mode: dirty
pages are attached to the **inode**, not a specific file descriptor, and a `close()` triggers a FUSE
`FLUSH` request — but per the Linux kernel's own FUSE writeback implementation, `FLUSH` does not
itself force synchronous writeback of dirty pages to the FUSE server; actual writeback is driven by
the kernel's ordinary dirty-page writeback path (`dirty_writeback_centisecs`, default 500 — a 5
second periodic sweep — and `dirty_expire_centisecs`, default 3000). A file that is written and
closed can therefore sit dirty-but-unflushed in the guest's page cache for up to that interval,
**independent of whether the writing process is still alive**.

The test's Case 1 loop (`i=0; while true; do i=$((i+1)); echo $i > /work/heartbeat; done`) opens,
writes, and closes `/work/heartbeat` every iteration — by the config's own "close to open" framing
this reads as if each close should flush, but the underlying FUSE mechanism does not guarantee that.
`wall_ms=800` gives well under one writeback interval before the guest process is killed; the
1-second gap between `v1` and `v2` is also under the ~1–5 second window `dirty_expire_centisecs`/
`dirty_writeback_centisecs` describe. Both reads landing empty is consistent with the guest's dirty
pages never having been swept to virtiofsd at all in that window — a caching-timing artifact of the
proof mechanism, not evidence about the workload's actual liveness.

**This is reasoning from the active configuration and documented kernel/virtiofs semantics, not a
mechanism traced through Kata/virtiofsd's own source or confirmed by a controlled live experiment**
(varying `wall_ms` against a fixed writeback interval, or reading `/proc/[pid]/status`-equivalent
guest-side dirty-page state) — §4 explains why that experiment could not be run this pass.

## 3. The fix

Added `sync` immediately after each heartbeat write in both Case 1 and Case 2's guest scripts:

```
i=0; while true; do i=$((i+1)); echo $i > /work/heartbeat; sync; done
```

`sync` (the coreutils/busybox applet, calling `sync(2)`) forces the guest kernel to write back all
dirty pages system-wide immediately, not just the ones belonging to `/work/heartbeat` — acceptable
here since the guest workload owns nothing else worth flushing, and busybox ships `sync` as a
standard applet (no additional guest tooling required). This makes the heartbeat's host-visibility
synchronous with the guest write, independent of `virtio_fs_cache`'s own timer, restoring the
property the test actually needs: "stopped changing" is evidence about the guest process's liveness,
not about virtiofs cache timing. Case 2's placement (`echo $i > .../heartbeat_case2; sync; echo
AAAAAAAAAA; ...`) preserves Slice-5's own established write-then-flood ordering (red-team finding #6,
ADR-089/ADR-109 lineage) — the heartbeat write and its flush both complete before the
potentially-blocking flood write each iteration.

The test file's own top-of-file disclosure comment (stale since ADR-109 landed — it still read "NOT
independently executed against a live deployment this session (none reachable)" despite ADR-109's own
25/26 live run) is corrected to state the real, current status: run live once (ADR-109), one real
failure diagnosed and reasoned-fixed here (ADR-110), not yet re-verified live.

## 4. A separate, real, blocking finding: `ctr images mount` is broken in this WSL2 session

Attempting to re-run the test to verify the fix, `KataBackend::create()` failed immediately for
**every** case — before reaching the heartbeat logic at all, an unrelated regression from ADR-109's
own clean 8/8 · 22/22 · 25/26 · 12/14 results earlier the same day. Root-caused via a debug build with
the error string surfaced (the abuse-corpus test itself does not print `create()`'s failure detail):

```
kata_backend: ctr images mount failed: ctr: failed to mount [{bind /run/containerd/
io.containerd.mount-manager.v1.bolt/t/1/1  []}]: mount source: "/run/containerd/
io.containerd.mount-manager.v1.bolt/t/1/1", target: "...lower", fstype: bind, flags: 0, data: "",
err: no such device
```

Isolated with a direct, minimal reproduction (`sudo ctr images mount docker.io/library/busybox:latest
/tmp/testmount`, no AgentEngine code involved at all): containerd's `io.containerd.mount-manager.v1.bolt`
plugin registers a bind-mount record (`t/<n>/1`) but never creates the corresponding source directory
under `/run/containerd/io.containerd.mount-manager.v1.bolt/t/`, so the client-side bind-mount syscall
`ctr images mount` performs fails with `ENOENT`/`ENODEV`. Ruled out as environment flakiness, not a
one-off: reproduced identically after `systemctl restart containerd`, after deleting and letting
containerd recreate `/run/containerd` outright, and after a full `wsl --shutdown` (clears all VM-level
state, not just the service). Ruled out as a Kata-specific or general-containerd problem: `sudo ctr run
--rm docker.io/library/busybox:latest smoketest echo hello` — containerd's normal, non-`kata`,
non-custom-rootfs container path — succeeds cleanly every time. The raw overlay-mount mechanism
(`mount -t overlay ...`) also works standalone, and `ctr snapshot ls`/`ctr content ls` show the
image's content and snapshot are both present and committed. The defect is isolated specifically to
the standalone `ctr images mount <image> <target>` CLI subcommand's own mount-manager bind-source
provisioning — the exact mechanism `KataBackend::create()` depends on (`src/backends/kata/
kata_backend.cpp:750`) to build its `--config`-mode custom rootfs, per SLICE 9's own reasoning for why
it cannot use containerd's normal snapshot-mount-list RPC path for Kata.

This blocks re-verifying **this ADR's own fix**, and blocks every other Kata test in the suite from
running live in this session — a materially larger problem than the one this ADR set out to close,
surfacing less than a day after ADR-109's own "first real deployment run" success. Not diagnosed
further this pass (containerd 2.2.1's `mount-manager` plugin internals are out of scope for what this
ADR was asked to investigate); named as a residual, §5.

## 5. Decision

Land the reasoned `sync` fix and the corrected disclosure comment — the diagnosis is well-supported by
the active config and documented FUSE/virtiofs semantics, and the fix is minimal, test-only, and does
not weaken what the containment proof demonstrates. Per explicit direction: apply now, disclosed as
unverified, rather than block on an environment repair of unknown scope. **Do not claim this closes
ADR-109 §8** — it closes the reasoning, not the live evidence.

## 6. Residuals

- **The `sync` fix is unverified against a live deployment.** The next session with a working Kata
  environment should re-run `test_kata_backend_abuse_corpus_linux` and confirm Case 1 and Case 2 both
  show non-empty, unchanged `v1`/`v2` post-fix. If the failure persists, the diagnosis in §2 is wrong
  and needs revisiting (candidates not yet ruled out: `virtiofsd`'s own buffering ahead of the guest
  kernel, or the `ctr tasks kill` signal path not actually delivering before the heartbeat read).
- **`ctr images mount`'s mount-manager defect (§4) is unresolved and blocks the whole Kata suite**,
  not just this test. Real, tractable follow-on work: bisect against a fresh Kata provisioning run
  (per `docs/planning/kata-backend-ci-runner-setup.md`) to determine whether this is a containerd
  2.2.1 regression, a WSL2-specific interaction, or a config drift specific to this session's
  environment; a `containerd` downgrade or a different snapshotter may be the fastest workaround if
  root-causing the plugin itself proves expensive.
- **CNI/`cnitool` still not installed** (ADR-109 §8's own already-disclosed residual, unchanged by
  this ADR) — carried forward, not touched here.
