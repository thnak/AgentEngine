# 2026-08-24 — `ctr run`'s `--config` flag is fully exclusive of every convenience flag

## Question

`KataBackend::create()` (`src/backends/kata/kata_backend.hpp`'s own header comment, Slice 2) already
named `ResourceLimits::pids` as unenforced because `ctr run` has no direct `--pids-limit` convenience
flag, and speculated the fix would need "a full OCI `config.json` via `ctr run --config`". Before
committing to that as a real Slice 6 design, this note pins down exactly what `--config` does —
verified against containerd's real, current source, not assumed.

## Sources consulted

- `github.com/containerd/containerd/cmd/ctr/commands/run/run_unix.go` (`main` branch, fetched
  2026-08-24 via `raw.githubusercontent.com`, saved locally for direct `grep`/`Read` rather than
  relying on a summarizing fetch)
- `github.com/containerd/containerd/cmd/ctr/commands/run/run.go` (same branch, same date)
- `github.com/containerd/containerd/cmd/ctr/commands/commands.go` (`ContainerFlags`, same date)

## Findings

1. **No `--pids-limit` flag exists anywhere in the current `ctr` CLI.** Checked exhaustively across
   `run.go`'s command-level flags, `run_unix.go`'s `platformRunFlags` (which does have `--cpus`,
   `--cpu-shares`, `--cpuset-cpus`, `--cpuset-mems`, and — notably — `--rlimit-nofile`, directly
   relevant to `ResourceLimits::fds`), and `commands.go`'s shared `ContainerFlags` (`--memory-limit`,
   `--mount`, `--net-host`, `--gpus`, etc.). `pids` has no convenience-flag equivalent at all.
2. **`--config <path>` is a fully separate, non-merging code path**, confirmed directly from
   `run_unix.go`'s `NewContainer()`:
   ```go
   if config {
       cOpts = append(cOpts, containerd.WithContainerLabels(...))
       opts = append(opts, oci.WithSpecFromFile(cliContext.String("config")))
   } else {
       // ~300 lines: oci.WithDefaultSpecForPlatform, withMounts(cliContext), image pull/unpack/
       // snapshot creation, oci.WithImageConfig, memory-limit (line 364), cpu-quota (line 330),
       // rlimit-nofile (line 416), process args/cwd/user/tty, everything else `ctr run` normally
       // builds from flags -- ALL of it lives inside this `else` branch, none of it runs when
       // `config` is true.
   }
   ```
   The `if`/`else` is a hard branch, not a base-spec-plus-overrides layering. When `--config` is
   passed, `--mount`, `--memory-limit`, `--cpus`/`--cpuset-*`, `--rlimit-nofile`, `--env`, `--user`,
   image resolution, and snapshot/unpack ALL DO NOT RUN. The loaded spec file is the entire spec —
   nothing this backend currently relies on via convenience flags carries over automatically.
3. **Argument shape differs too**: `if config { id = Args().First() } else { ref = Args().First();
   id = Args().Get(1) }` — in `--config` mode, `ctr run --config <path> <id>` takes only a container
   ID, no image reference at all. Image pull/unpack/snapshot preparation is not something `--config`
   mode does implicitly; a caller using it is expected to have already arranged a rootfs (via the
   spec's own `root.path`, or a pre-existing snapshot mount) before invoking `ctr run --config`.
4. **Structural reason a host-side workaround can't substitute**: Kata's isolation boundary is a real
   VM — the guest kernel manages its own process table independently of the host. A host-side cgroup
   update against the VMM/shim process tree (e.g. some hypothetical `ctr tasks update`-style call)
   would constrain host-visible processes, not what's running *inside* the guest. Only a resource
   limit the guest's own container runtime (kata-agent, inside the VM) applies to the guest-internal
   cgroup — i.e. `linux.resources.pids.limit` in the OCI spec kata-agent receives — can actually bound
   guest-internal forking. There is no host-side shortcut for this specific containment property.

## Consequence for a `pids` enforcement design

Real `pids` enforcement for `KataBackend` is not "add one more flag alongside the existing
`--mount`/`--memory-limit` args" — it requires either:

- **(a)** Switching `create()` entirely to `--config` mode: hand-authoring a full OCI runtime spec
  (process args/cwd/user, `root.path` pointing at an already-prepared rootfs, all mounts, memory AND
  now pids resource limits, kata-runtime-specific annotations) and separately orchestrating image
  pull/unpack/snapshot creation via other `ctr`/containerd calls first, since `--config` mode does
  none of that itself — reimplementing, by hand, most of what the convenience-flag path already does
  for us today, entirely unverifiable against a live Kata deployment (none reachable this session).
- **(b)** Some other, not-yet-identified mechanism (a newer `ctr` version's flag, a containerd Task
  API call issued via a lighter client than the full `ctr run` command, or accepting a materially
  different/looser scope for what "pids enforcement" means for this backend) — not investigated
  further as part of this note; flagged as the fork in the road before committing engineering effort.

This is a materially larger and riskier undertaking than the original Slice 2/3/4/5 header-comment
framing ("would need a full OCI config.json") fully conveyed — it's not an additive change to the
existing `create()` path, it's a replacement of most of it, with zero live-deployment verification
available. Recorded here rather than discovered mid-implementation.
