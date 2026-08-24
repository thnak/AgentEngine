# ADR-091 — KataBackend: `ResourceLimits::fds` via `ctr run --rlimit-nofile`

Status: Proposed (2026-08-24; implemented, local verification complete, awaiting project-owner
Judged sign-off)

## 1. The question

`decisions/ADR-090-kata-backend-pids-limit-investigated-and-deferred.md`'s own investigation into
`ResourceLimits::pids` surfaced, as a byproduct, that `ctr run --rlimit-nofile` genuinely exists as a
convenience flag (`platformRunFlags`, confirmed against containerd's real source) — unlike `pids`,
closing `ResourceLimits::fds` needs no `--config` rewrite, no snapshot/chain-ID problem, none of
ADR-090's blocking issues. This ADR is that real, much smaller fix, landed the same session it was
found rather than left to rediscovery.

## 2. The fix

`KataBackend::create()`'s `limit_args` construction gains one more conditional flag, alongside the
existing `memory_bytes` → `--memory-limit` mapping:

```cpp
if (spec.limits.fds > 0) {
    limit_args.push_back("--rlimit-nofile");
    limit_args.push_back(std::to_string(spec.limits.fds));
}
```

A bare `<n>` (no `soft:hard` colon) sets both the soft and hard `RLIMIT_NOFILE` to the same value —
confirmed directly against containerd's real `run_unix.go` parsing (`strings.Cut(c, ":")`; `!found`
falls back to `hardS = softS`), not guessed from `ctr run --help` usage text alone.

## 3. What this does and does not prove

`oci.WithRlimit` (the SpecOpt this flag drives) sets `process.rlimits` on the OCI spec — which governs
the container's own **initial process** for certain (this backend's `sleep infinity` placeholder,
started once at `create()` time). Whether a **later** `ctr tasks exec`-spawned process — this
backend's actual per-call workload path, used for every real `exec()` call — inherits that same limit
is a genuinely open question this ADR does **not** resolve by inspection: the OCI runtime's `exec`
operation may or may not re-derive rlimits from the container's original process spec, and nothing in
containerd's `ctr` CLI source settles it either way without checking the actual runtime/kata-agent
exec-path behavior against a live system.

**Disclosed, not assumed — and not silently left unaddressed either.** A new test case
(`tests/test_kata_backend_slice2_linux.cpp`, case 6) targets exactly this question directly: with
`spec.limits.fds = 123` set at `create()`, it execs `ulimit -n` and asserts the exec'd process's own
reported limit is exactly `123` — real, falsifiable evidence either way, the first time this repo runs
it against a live deployment, rather than a claim resting on inspection alone. The test's own comment
says explicitly that a failure here is real information, not a flake to retry past.

## 4. Verification performed this session

- Compile-verified via the same WSL Ubuntu `build-linux` real g++/cmake/ninja cache used by every
  prior Kata slice this session: `agentengine_kata_backend` and `test_kata_backend_slice2_linux`
  rebuild clean.
- No live Kata/containerd deployment reachable this session — the new test's actual verdict (does
  `fds` propagate to `exec()`-spawned processes or not) remains unknown until a future session with
  real deployment access runs it. This ADR does NOT claim the propagation question is answered —
  only that the mechanism is wired and the test to answer it honestly now exists.

## 5. Residuals

- Whether `fds` reaches `ctr tasks exec`-spawned processes is unresolved (§3) — the single most
  important open fact this ADR leaves for a future session with real deployment access.
- `ResourceLimits::pids`/`disk_bytes`/`net_bytes` remain unenforced (`disk_bytes`/`net_bytes`
  uninvestigated; `pids` investigated and deferred, ADR-090).
