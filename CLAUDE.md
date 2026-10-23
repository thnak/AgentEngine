# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

**AgentEngine** is a C++23 engine for building agent applications: agents, sessions, tools, and
multi-agent workflows on its own `agentengine::rt::` runtime substrate (ADR-037 removed the prior
[Quark](https://github.com/thnak/QuarkCpp) dependency; `third_party/quark` is gone from the tree),
with sandboxed execution and a Python code interpreter as built-in subsystems, speaking the open
agent protocols of 2026 (MCP, A2A, AG-UI, OpenTelemetry GenAI).

The developer model is **MAF-shaped** (Microsoft Agent Framework's agent/session/tool/workflow
vocabulary), expressed as a **C++23 API designed for least developer code** (`ADR-242` R5: CRTP is
optional authoring sugar, type erasure is fine off measured hot paths). The target is developers
building multi-user agent servers on Linux; Windows is supported for development and desktop.

**The project is spec-driven.** 30 RFC documents (`rfcs/NNN-*.md`) are the authoritative design;
`decisions/ADR-*.md` are executed proofs (the live record starts at ADR-242; ADR-001 to ADR-241 are
in `decisions/archive/`, and `decisions/README.md` says which of them still bind). **When code and a spec disagree, the spec wins**; if the
spec is wrong, fix the spec first (with an ADR), then the code.

**Status: implementation under way, not design phase.** All 30 RFCs passed their own review gate
(`docs/planning/v1-review-signoff-workflow.md`); real, tested C++23 implementation exists under
`include/agentengine/`, `src/`, and `tests/`, with ADR evidence behind specific gates (`decisions/`).
**Do not hardcode a milestone/phase table here — it goes stale fast.** Check
`docs/planning/v1-implementation-roadmap.md` and the per-milestone breakdown docs for current
phase/gate status.

## Read first

1. **[AgentEngineSpecification.md](AgentEngineSpecification.md)** — vision, layering, the eight core
   invariants, the locked decisions, the runtime substrate.
2. **[CONVENTIONS.md](CONVENTIONS.md)** — the binding coding contract.
3. **[README.md](README.md)** — the RFC index and reading order.
4. **[OpenQuestions.md](OpenQuestions.md)** — what is unresolved and what it blocks.

## The invariants that constrain every change

I1 one session, one executor · **I2 no ambient authority** · **I3 model output is data, never
authority** · I4 every effect is attributable · I5 nondeterminism crosses a recorded seam ·
I6 declarative and native surfaces are equivalent · I7 protocol conformance is a gate ·
I8 budgets are enforced. Full statements in the specification, §4.

I2 and I3 are the ones most easily broken by a convenient-looking change. If a change makes it
possible to reach an effect without an explicitly passed capability, or lets anything derived from
model output influence a permission decision, it is wrong regardless of how well it reads.

## Locked decisions — do not relitigate without an ADR

`ADR-242` (Judged 2026-10-20) is the project's direction; read its §2, §3 and §3a before any
architectural change.

- **Nothing is chosen implicitly; the developer chooses and owns the risk; invariants are never
  options** (`ADR-242` §2). The engine ships managed options for every resource (execution backend,
  authority source, budgets and limits, turn cap, approval, concurrency, telemetry, strictness,
  Runtime, secrets, transport). If one is not chosen, startup fails with a named error listing the
  options. A preset is an explicit choice of everything it bundles. "Explicitly unbounded" is a valid
  budget value when stated as a named value, never as `0` or a missing field. "Own risk" covers the
  residuals each option discloses in `ADR-242` §3a; it never relaxes I1–I8. Keep §3a true: a cell
  says "engine enforces" only when an enforcement test with a positive control proves it.
- **Every agent runs inside a sandbox from a `SandboxBackend`, and every backend hosts the worktree**
  (`ADR-242` R8). The worktree is always present in the backend and stays engine-owned and durable
  (D4). "No sandbox" is `NoSandboxBackend`, a real conformer that is never `Strict`-eligible.
- **Execution options** (`ADR-242` R2): `microvm` (Kata, with its own `bash`/`python3`),
  `native-jail` (Windows, Linux), `remote`, `wasm`, and `NoSandboxBackend`; none is the default, and
  no fallback is silent. Under `microvm`, I2 is enforced at the VM boundary, from the host
  (`ADR-242` R3 rules 1–7).
- **The shell and the CodeAct interpreter are two tiers, each attached by a `ContextProvider`**
  (`ADR-242` R3). `execute_code` (persistent interpreter + `agent.*`) exists only when its provider is
  attached. CPython is the language wherever it runs; never a WASI Python. `ADR-071`'s
  `NativePythonProvider` stays distinct and is never substituted for `execute_code`.
- **An optional `agentengine_server` module** (`ADR-242` R4) may provide the MCP/A2A/AG-UI transports
  and a listener; the core stays transport-agnostic and a host may still own its socket. Per-request
  authority is mandatory in the server module.
- **One host-owned Runtime per process, passed explicitly** (`ADR-242` R6); library code creates no
  threads or private Runtimes.
- **Quark is not a dependency of this project.** `ADR-037` removed it entirely; the runtime substrate
  is `agentengine::rt::`. Do not reintroduce without a new ADR.
- **WASM Component Model (WASI 0.3) is the plugin ABI** — tools, skills, providers, memory stores,
  filters, and the C/C++ library track (009 §7).
- **Python/.NET bindings are deferred.** v1 authoring surfaces are the C++ API and declarative
  YAML/JSON (I6).
- **The licence is MIT, decided.** See [`LICENSE`](LICENSE); resolved 2026-08-04
  (024 Q1 / OQ-11). Not an open question.

## Feature vs. safety balance

**Enable features through explicit choices, not by weakening invariants.** `ADR-070`'s Delegated
Decision Seam (explicit opt-in, narrows or decides among already-possessed authority only, host code
never model output, audited) is how responsibility for an already-scoped risk moves to the developer;
`ADR-242` §2 applies it to every resource and strengthens "unset" from "today's behaviour" to a named
startup error. Ship behind an explicit choice with honestly disclosed residuals (in `ADR-242` §3a),
harden later in a follow-on ADR. This is not a license to relax I2/I3 themselves — `ADR-070` §3
rejected that outright — and the identity-to-authority policy is the developer's root of trust, not a
seam (`ADR-242` R7).

## Working within this repo

- **Research is dated and cited.** External claims go in `docs/research/<date>-<topic>.md` with
  sources. Do not assert what a protocol does from memory — these moved twice in eight months
  (MCP's stateless-core revision is dated 2026-07-28).
- **Every RFC has a promotion gate.** A design without a falsifiable gate does not get written down
  as settled. Security claims need positive controls — a test that cannot fail proves nothing.
- **Contested, hot-path, or security-critical designs go through `design → red-team → prove →
  judge`** and produce an ADR in `decisions/` (numbered from 243, kept short: a few hundred lines,
  work logs in `docs/planning/`), not an ad-hoc change. Quark's agents
  (`quark-architect`, `quark-redteam`, `quark-prover`, `quark-judge`) are the model for this.
- **When implementation starts**: every source file names the spec(s)/ADR(s) it implements in a top
  comment; a load-bearing invariant without a test, a hot path without a bench, or a protocol claim
  without a conformance run is not done.

## Machine safety

**Sandbox and hostile tests are resource-capped** — a test proving a fork bomb is contained must not
be able to take the machine with it.

## Git

**`origin` is a real remote** — <https://github.com/thnak/AgentEngine> — so `git push` has a target
and pushing is a genuine, outward-facing action. This line previously read "Local repository, no
remote", which stopped being true once the project was published; a session that trusts it will
reach for the wrong default when asked to push. **Commit messages carry no co-author trailer**
(explicit project owner instruction).
