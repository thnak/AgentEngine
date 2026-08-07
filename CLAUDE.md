# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

**AgentEngine** is a C++23 engine for building agent applications: agents, sessions, tools, and
multi-agent workflows on top of the [Quark](https://github.com/thnak/QuarkCpp) actor engine, with
sandboxed execution and a Python code interpreter as built-in subsystems, speaking the open agent
protocols of 2026 (MCP, A2A, AG-UI, OpenTelemetry GenAI).

The developer model is **MAF-shaped** (Microsoft Agent Framework's agent/session/tool/workflow
vocabulary) expressed in **Quark's zero-cost CRTP policy idiom**.

**The project is spec-driven.** 30 RFC documents (`NNN-*.md`) are the authoritative design;
`decisions/ADR-*.md` are executed proofs. **When code and a spec disagree, the spec wins**; if the
spec is wrong, fix the spec first (with an ADR), then the code.

**Status: implementation under way, not design phase.** All 30 RFCs have passed their own review
gate (each RFC's own `**Status:**` header reads `Reviewed`, dated 2026-08-05 —
`docs/planning/v1-review-signoff-workflow.md`); several have real ADR evidence behind specific
gates (see `decisions/`). Real, tested C++23 implementation exists under `include/agentengine/`,
`src/`, and `tests/`. Per `docs/planning/v1-implementation-roadmap.md`'s milestone sequence,
Milestones 0-4 are complete and Milestone 5 is in progress (Phases A-I done; Phase J, the
milestone's own exit-criterion proof, remains). Milestones 6-9 have not started. **Do not hardcode
a milestone/phase table here** — it goes stale on the next commit. The live, accurately-maintained
source of truth is `docs/planning/v1-implementation-roadmap.md` (the milestone plan) and the
per-milestone `docs/planning/milestone-*-breakdown.md` files (phase-by-phase progress with
verification notes) — check those, not this file, for current status.

## Read first

1. **[AgentEngineSpecification.md](AgentEngineSpecification.md)** — vision, layering, the eight core
   invariants, the three locked decisions, the Quark mapping.
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

- **Quark is a submodule, never forked or patched in-tree.** Runtime changes go upstream.
- **WASM Component Model (WASI 0.3) is the plugin ABI** — tools, skills, providers, memory stores,
  filters, and the C/C++ library track (009 §7).
- **The Python code interpreter is embedded native CPython, permanently — never WASM, never a
  second runtime**, regardless of WASI Python's future maturity. The rich Python-on-WASM ecosystem
  is Emscripten (needs a JS host) and WASI Python has no binary-wheel ecosystem yet (evidence:
  `docs/research/2026-standards-landscape.md` §6) — corroborating, not the reason: two Python
  runtimes would mean an agent's generated code, and the humans verifying it, must know which one
  they're dealing with. The sandbox seam (008) still lets the *isolation backend* evolve; the
  interpreter itself does not fork.
- **No `microvm` sandbox profile.** Isolation strength for the interpreter/shell comes from treating
  the whole execution environment as the sandbox — worktree, capabilities, resource limits, network
  policy — with CPython's dangerous entry points mediated at the point of use and the OS-level jail
  as a second layer (008 §1b), not from a second local isolation technology. A workload that would
  need hardware isolation uses the `remote` profile against infrastructure that already provides it.
- **v1 authoring surfaces are C++ CRTP and declarative YAML/JSON.** Python/.NET bindings deferred.

## Working within this repo

- **Research is dated and cited.** External claims go in `docs/research/<date>-<topic>.md` with
  sources. Do not assert what a protocol does from memory — these moved twice in eight months
  (MCP's stateless-core revision is dated 2026-07-28).
- **Every RFC has a promotion gate.** A design without a falsifiable gate does not get written down
  as settled. Security claims need positive controls — a test that cannot fail proves nothing.
- **Contested, hot-path, or security-critical designs go through `design → red-team → prove →
  judge`** and produce an ADR in `decisions/`, not an ad-hoc change. Quark's agents
  (`quark-architect`, `quark-redteam`, `quark-prover`, `quark-judge`) are the model for this.
- **When implementation starts**: every source file names the spec(s)/ADR(s) it implements in a top
  comment; a load-bearing invariant without a test, a hot path without a bench, or a protocol claim
  without a conformance run is not done.

## Machine safety

The dev box can hang or power off if a build saturates its cores. Build with `-j4` max (TSan `-j1`);
pin tests and benchmarks to ≤ 4 cores; never spawn `hardware_concurrency()` threads. **Sandbox and
hostile tests are resource-capped too** — a test proving a fork bomb is contained must not be able
to take the machine with it.

## Git

Local repository, no remote. **Commit messages carry no co-author trailer** (explicit project
owner instruction).
