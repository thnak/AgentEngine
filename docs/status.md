# Project status: what is built, what you can use, what comes next

**As of 2026-10-23** (`main` at the merge of PR #222; the YAML capabilities block has merged). This page is a
dated snapshot, not a spec. When it disagrees with an ADR or a test, the ADR and the test win; every claim below
cites where to check it. It is kept honest the way ADR-242 §3a is: a thing is listed as "enforced" only when a test in
this repository proves it.

## The short answer

**AgentEngine is not ready to run an untrusted, multi-user, internet-facing agent server.** It is a real, tested C++23
engine you can build agent applications on today, with the choices that matter stated explicitly, but the server
module, the tenancy layer and the hardened Linux execution path that such a deployment needs are not built
(ADR-242 §6 steps 3-6). Use it now for prototypes, local and desktop tools, evaluation, and host applications where
you trust the caller. Do not use it yet where the person at the other end is an adversary.

## Evidence snapshot

| What | Result | Where to check |
|---|---|---|
| Platforms built in CI | Windows (MSVC, clang-cl, each Release and sanitized), Linux (gcc-14) | `.github/workflows/ci.yml` |
| Test suite | about 445 tests on Linux gcc, 519 on Windows MSVC, on the 2026-10-23 tree | `ctest` |
| Known environment-only failures | 3 containerd tests (they need the containerd socket, and pass under `sudo`), `test_json_dump_escape` (a memory-cap check that depends on the host) | [thnak-server runbook](guides/thnak-server-runbook.md) |
| Required-choices work (ADR-244) | Judged 2026-10-23. Slices 1-5 and part of 6 are merged (PR #222 landed the stated defaults); the YAML `capabilities:` block (PR #223) and the time-limit row are done | [ADR-244](../decisions/ADR-244-required-choices-step-1b.md) §5 |
| Direction (ADR-242) | Judged 2026-10-20 | [ADR-242](../decisions/ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md) |
| Public API and layers (ADR-243) | Proposed, not Judged; the prove phase runs per migration slice | [ADR-243](../decisions/ADR-243-layer-and-developer-api.md) |
| Protocol conformance | MCP client role, measured 2026-08: non-auth 75/75, auth 3/49 (no OAuth client yet), so the MCP gate is **not met** | [README](../README.md), RFC 011 §10 |

## What you can use today

"Enforced" means a test with a positive control proves it (ADR-242 §3a). "Disclosed" means the engine does not stop
you, and the cost is yours to manage.

### Choices the engine makes you state (ADR-244)

A missing choice is a compile error or a named startup error, not a default. Enforced:

- Session authority source, turn cap, token budget, approval mode, concurrency mode, telemetry capture and
  sandbox strictness (`SessionChoices`; `MaxTurns`, `TokenBudget`, `Approval`, `Concurrency`, `SandboxProfile` tags).
- Every capability cap (`cap::FsRead`, `FsWrite`, `NetOut`, `Exec`, `NativeExec`) as a `Limit`: a number or the named
  `unbounded()`, never `0` and never omitted. Engine floors on the host-safety bounds kept under `unbounded()` are
  named and recorded on the sandbox "create" event (`core/engine_floor.hpp`).
- The secret environment prefix and a registered (not defaulted) sandbox backend.
- Plugin manifest limits, scope concurrency, the history window (`UnboundedWindow`), the clock resolution and an empty
  mount grant prefix (PR #222).
- A declarative agent's capability ceiling in YAML (`capabilities:`), parsed strictly into the same values the C++
  form produces (PR #223).
- **In review, not yet on `main`:** per-call time limits: `Deadline`, `SessionChoices::call_timeout_ms`, and
  `HttpOptions::total_timeout_ms` are stated choices.

Two of those values are **declared only**: `MountSpec::quota_bytes` and `ClockResolution` are accepted but no
backend applies them yet (ADR-242 §3a).

### Execution backends

| Backend | State | Use it for | Cost you carry |
|---|---|---|---|
| `native-jail`, Windows | Works; the CodeAct interpreter is wired only in the dev CLI | Desktop and development | Can still read OS system paths outside your grants; set every limit |
| `native-jail`, Linux | Backend works (namespaces, `pivot_root`, seccomp, cgroups v2) | Commands without Python, on hosts without KVM | Same kernel, not hardware; no interpreter yet |
| `microvm`, Kata (Linux + KVM) | Backend works, live-verified 2026-10-20; not wired to the shell or CodeAct tools | The strongest boundary available, once you wire a shell provider | Engine process runs as root today; VMs are never reaped; `NetPolicy::allowlist` and `net_bytes` do not filter microVM traffic (issue #194); no digest-pinned image yet |
| `wasm` | Works for WASI components | Plugins and WASI tools | No shell or Python |
| Docker and containerd shell surfaces | Work | Persistent shells | The command runs with whatever authority your code holds; no capability is checked |
| ADR-071 native providers | Work, explicit opt-in | Trusted local automation | Code runs on the host as the engine's user |
| `remote` | Stub | Nothing yet | Everything |
| `NoSandboxBackend` | Not implemented | Nothing yet | Everything |

### Everything else

- **Agents, sessions, tools, workflows, handoff, group chat, checkpoint and resume:** built and tested, with 35
  examples, most offline against a scripted model ([`examples/`](../examples/)).
- **Real providers** (the OpenAI and Anthropic chat clients, which also reach OpenAI-compatible endpoints, an embedder and
  a Qdrant index) need
  `AGENTENGINE_WITH_HTTPS`; live-network tests skip without credentials.
- **Persistence:** `rt::SessionStore` and `rt::AppendLogStore` with a file store. A corrupt record mid-file loses what
  follows it (spec §7), and backups and replication are yours.
- **Protocol surfaces** (MCP, A2A, AG-UI) are typed dispatchers over already-parsed requests. There is no HTTP
  listener by design (ADR-061); the A2A wire binding does not exist.

## What stands between this and a production server

Each item is a named gap, not a guess. The authoritative list is [ADR-242](../decisions/ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md) §3a and §7.

1. **No server module.** You own the socket, TLS, HTTP parsing and authentication (R4, step 5).
2. **No tenancy layer.** Identity-to-authority policy is the developer's, and the per-tenant secret namespace is flat (R7, step 6).
3. **One execution seam is unfinished.** Docker and containerd surfaces check no capability, `NoSandboxBackend` is not
   built, the CodeAct tier is not attached over Kata or the Linux jail (step 3, step 3b).
4. **Kata operational limits.** Root requirement, no VM reaping or recycling, cold start of about 6 s, density tested
   only to 40 VMs, no pinned guest image (ADR-242 §7).
5. **Runtime and threads.** Provider clients build a private runtime if you do not pass one, and streaming and MCP
   background tasks use detached threads, so bound concurrency yourself (ADR-237, issues #180, #182).
6. **A time limit is a boundary check, not preemption.** Each call has a stated deadline (`SessionChoices::call_timeout_ms`),
   but a tool body that is already running is not interrupted (ADR-244 §5). The HTTP `idle_timeout` and
   `consumer_stall_timeout` still default to 90 s.
7. **MCP gate not met** (no OAuth client), and the A2A task store never evicts.

## Plans, in order

| Order | Work | State |
|---|---|---|
| 1 | Finish ADR-244 (Docker and containerd isolation limits `cap::UnmediatedShell` and the containerd host bind are done): pinned images for the Docker and containerd surfaces (Kata is done) | In progress; the rest needs live Linux hosts to prove |
| 2 | Finish ADR-237: the runtime substrate and removal of the legacy synchronous machinery (issues #175-#187, #196) | Open; awaiting Judge sign-off (#186) |
| 3 | Public API and layer redesign (ADR-243): the type-erased session and chat client, session and workflow out of `rt/`, the line budget | Proposed |
| 4 | One execution seam (ADR-242 step 3): worktree verbs in `SandboxBackend`, Docker and containerd as conformers, `NoSandboxBackend`, the CodeAct tier over Kata, the pinned Kata guest image, recycling | Not started |
| 5 | A Linux native-jail Python worker (step 3b), so a Linux host without KVM has a local `execute_code` | Not started |
| 6 | Server module (R4) and tenancy layer (R7) | Not started |
| Alongside | MCP OAuth client to meet the conformance gate; an optional DB-backed worktree store (issue #219) | Open |

Steps 2-4 of ADR-242 touch the same headers, so they are sequenced, not run in parallel.

## Keeping this page true

This page is rewritten when a slice of ADR-242, ADR-243 or ADR-244 lands, using the same rule as ADR-242 §3a: a row moves
to "enforced" only with a named test that has a positive control. The per-milestone detail lives in
[`docs/planning/`](planning/), and `tools/milestone_status_lint.py` guards the "have not started" sentences in the
README and the site against drifting from those milestone docs.
