# ADR-090 — KataBackend: `ResourceLimits::pids` enforcement investigated, deferred

Status: Proposed (investigation complete, decided in the negative; awaiting project-owner sign-off,
mirroring ADR-088 §2's own "investigated and rejected" precedent for `exec_outcome_class::oom`)

## 1. The question

`kata_backend.hpp`'s own header comment has named `ResourceLimits::pids` as unenforced since Slice 2
(ADR-086): "`pids` has no direct `ctr run` CLI flag this pass wires (would need a full OCI
`config.json` via `ctr run --config` instead of the convenience flags used here — a real, scoped-out
follow-on)." This ADR is that follow-on investigation, run to a real conclusion — a decision, not
another deferral.

## 2. What was actually verified, and against what

Every claim below was checked against containerd's real, current source
(`github.com/containerd/containerd`, `main` branch, fetched 2026-08-24), not assumed —
`docs/research/2026-08-24-containerd-ctr-run-config-vs-convenience-flags.md` has the full source
citations and quoted code. Summary of the investigation's escalating findings:

1. **No `--pids-limit` (or equivalent) convenience flag exists anywhere in the current `ctr run`
   CLI** — checked exhaustively across `run.go`'s command-level flags, `run_unix.go`'s
   `platformRunFlags` (which does have `--cpus`/`--cpu-shares`/`--cpuset-cpus`/`--rlimit-nofile`),
   and `commands.go`'s shared `ContainerFlags` (`--memory-limit`, `--mount`, etc.).
2. **`--config <path>` is the only way to set `linux.resources.pids.limit`, and it is fully
   exclusive of every convenience flag** — confirmed directly from `run_unix.go`'s `NewContainer()`:
   the `if config { ... } else { ... }` branch is a hard split, not layering. `--mount`,
   `--memory-limit`, image pull/unpack/snapshot creation, `--env`, `--user`, everything this
   backend's `create()` currently relies on via convenience flags, simply does not run when
   `--config` is set. The loaded spec file is the entire spec.
3. **`--config` mode does not prepare a rootfs at all** — with no `containerd.WithNewSnapshot`
   `NewContainerOpts` in that branch, `container.Mounts(ctx)` returns empty at task-creation time,
   so the runtime shim receives zero snapshot mounts and treats the spec's `root.path` as a literal,
   already-populated host directory. A caller using `--config` is expected to have already prepared
   one — `--config` mode has no equivalent of the implicit pull/unpack the convenience path performs.
4. **No post-creation resource-update path exists via the `ctr` CLI either** — `ctr tasks`'s real
   subcommand list (`cmd/ctr/commands/tasks/tasks.go`) is `attach, checkpoint, delete, exec, list,
   kill, metrics, pause, ps, resume, start` — no `update`. (The containerd Task gRPC service does
   have an `Update` RPC used internally elsewhere, but no `ctr` CLI subcommand wraps it.) This closes
   off a much lower-risk alternative design (create normally via the existing convenience-flag path,
   then patch just the pids limit afterward) before it could be attempted.
5. **`--runtime io.containerd.kata-clh.v2` still works in `--config` mode** — `containerd.WithRuntime`
   is applied unconditionally, after the `if`/`else` closes, common to both branches. This one piece
   is NOT a blocker.
6. **The remaining path — hand-preparing a rootfs directory via `ctr images pull` + `ctr snapshot
   unpack <digest>` + `ctr snapshot prepare --mounts <key> <parent>` — has a real information gap,
   not just added complexity**: `unpack` takes a manifest **digest**, not a human-readable ref, and
   internally creates a *committed* snapshot keyed by an internal, content-addressed chain ID that
   containerd computes but does not expose through any stable `ctr` CLI-level query — there is no
   `ctr` subcommand that answers "what snapshot name did unpacking image X just produce." Discovering
   it would mean scraping `ctr snapshot list`'s output against undocumented internal naming
   conventions, not calling a documented API. (Requesting the `native` snapshotter specifically would
   at least avoid a SEPARATE problem — its snapshots are plain, already-populated directories, so a
   `bind`-typed `Mount.Source` from `prepare` could be used directly as `root.path` with no additional
   `mount(8)` call needed — but that only sidesteps the mount-syscall risk, not this naming-discovery
   gap.)

## 3. Why this is a decision, not just "more work needed"

Finding 6 is qualitatively different from findings 1-3 (which are real but surmountable — more code,
more risk, still buildable). It is a genuine information gap in the CLI surface itself: the
containerd Go client library's own snapshot/image APIs return the chain ID directly as a normal
function return value (this is exactly how `image.Unpack()` and `containerd.WithNewSnapshot()` wire
together internally, inside the same process, in the convenience-flag path this backend does NOT
use). The `ctr` CLI process boundary — deliberately chosen for this backend precisely to avoid
vendoring a second RPC stack (`kata_backend.hpp`'s own header comment: "keeps this backend's own
dependency footprint to 'a `ctr`/containerd/kata-runtime install already on PATH'... avoiding
vendoring a second RPC stack into this C++ tree") — is exactly what stands between this backend and
that information. No amount of additional shell-scripting closes an information gap that exists
because the information was never serialized to a CLI output surface in the first place; only
scraping undocumented internals (fragile, unstable across containerd versions, and — critically —
untestable here, since no live Kata/containerd deployment is reachable this session to even
discover what today's actual naming convention is) would "work," and only by accident.

**Decision: `ResourceLimits::pids` stays unenforced for `KataBackend`.** Real enforcement needs one
of:

- Embedding a real containerd Go/gRPC client (or ttrpc, whatever containerd itself uses) as an actual
  new dependency of this backend — reopening the architecture decision `kata_backend.hpp`'s header
  comment already made deliberately, in the other direction, and not something to reverse
  incidentally as a side effect of one resource-limit axis.
- A future containerd CLI release actually adding a stable way to discover a chain ID or to set
  `pids.limit` directly (tracked nowhere by this project today — would need to be re-checked, not
  assumed, the next time this gap is revisited).
- Accepting the fragile scrape-and-hope approach, explicitly, with its instability and
  unverifiability named up front — rejected here as exactly the kind of "decorative, not real"
  containment this project's own conventions exist to catch before it ships.

This mirrors ADR-088 §2's own precedent for `exec_outcome_class::oom`: a real, investigated,
negative decision recorded on the record, not a silently-unattempted gap and not an implementation
built on a foundation this investigation could not actually verify was sound.

## 4. What changed in this pass

No source code changed. `kata_backend.hpp`'s "Still NOT done" list is updated to reflect that `pids`
is now an investigated-and-deferred gap (like `oom` already was), not merely an unattempted one, with
a pointer to this ADR and the research note for the full chain of findings.

## 5. Residuals, carried forward explicitly

- `ResourceLimits::fds` is untouched by this investigation but worth naming as a real, much smaller
  opportunity surfaced along the way: `ctr run`'s `--rlimit-nofile` convenience flag (in
  `platformRunFlags`, unlike `pids`) DOES exist and works within the existing convenience-flag path —
  no `--config` rewrite needed for that axis. Not implemented this pass (out of the scope this ADR
  was chartered to investigate), named here so a future session does not have to rediscover it.
- `ResourceLimits::disk_bytes`/`net_bytes` remain entirely unenforced and uninvestigated — this ADR
  did not examine either.
- `exec_outcome_class::oom`, the unbounded-output orphan risk (fixed, ADR-089), and
  `ExecRequest::source` Runner-mediation remain the same unchanged gaps prior ADRs already name.
- The research note's own disclosed limitation: every finding here was checked against containerd's
  real current source, not a live deployment — the actual behavior of `--config` mode, snapshot
  chain-ID naming, and `native` snapshotter availability in THIS project's specific deployment target
  were never empirically confirmed, because no such deployment is reachable this session. This ADR's
  conclusion rests on source-code-level analysis, which is the strongest evidence available here, but
  is explicitly named as one level short of empirical confirmation.
