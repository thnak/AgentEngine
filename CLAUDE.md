# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

**AgentEngine** is a C++23 engine for building agent applications: agents, sessions, tools, and
multi-agent workflows on its own `agentengine::rt::` runtime substrate (ADR-037 removed the prior
[Quark](https://github.com/thnak/QuarkCpp) dependency; `third_party/quark` is gone from the tree),
with sandboxed execution and a Python code interpreter as built-in subsystems, speaking the open
agent protocols of 2026 (MCP, A2A, AG-UI, OpenTelemetry GenAI).

The developer model is **MAF-shaped** (Microsoft Agent Framework's agent/session/tool/workflow
vocabulary) expressed in AgentEngine's own **zero-cost CRTP policy idiom** (originally modeled on
Quark's identical idiom).

**The project is spec-driven.** 30 RFC documents (`NNN-*.md`) are the authoritative design;
`decisions/ADR-*.md` are executed proofs. **When code and a spec disagree, the spec wins**; if the
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

- **Quark is not a dependency of this project.** `ADR-037` removed it entirely (was an unmodified
  submodule); `third_party/quark` is gone and the runtime substrate is `agentengine::rt::`. Do not
  reintroduce without a new ADR.
- **WASM Component Model (WASI 0.3) is the plugin ABI** — tools, skills, providers, memory stores,
  filters, and the C/C++ library track (009 §7).
- **The embedded native CPython interpreter is the one mediated code-interpreter path,
  permanently** — never WASM, never a second *interpreter* for `execute_code` (rationale and WASI
  Python's ecosystem gap: `docs/research/2026-standards-landscape.md` §6). `ADR-071` narrowed this
  from a blanket "never a second runtime" to permit `NativePythonProvider` — a distinct,
  explicitly-scoped, host-opt-in native-automation capability, never substituted for `execute_code`
  and kept out of `agentengine_python_runner`. Don't blur the two, or widen either scope, without a
  new ADR.
- **No `microvm` sandbox profile.** Isolation strength for the interpreter/shell comes from treating
  the whole execution environment as the sandbox — worktree, capabilities, resource limits, network
  policy — with CPython's dangerous entry points mediated at the point of use and the OS-level jail
  as a second layer (008 §1b), not from a second local isolation technology. A workload that would
  need hardware isolation uses the `remote` profile against infrastructure that already provides it.
- **v1 authoring surfaces are C++ CRTP and declarative YAML/JSON.** Python/.NET bindings deferred.
- **AgentEngine does not implement HTTP networking itself.** Project-owner direction (2026-08-15)
  settled the inbound MCP/A2A/AG-UI transport question: host/consumer code owns the socket,
  terminates TLS/auth, and hands the engine already-parsed requests — MAF-style — rather than the
  engine running its own listener. `ADR-061`'s Tier 1 (client-role principal derivation) and Tier 3
  (host-fronted server role's session-side admission mechanism and bearer-credential bridges) are
  both Judged (2026-08-20, project-owner sign-off) — but Tier 3 never built the actual test-fixture
  listener a server-role conformance run needs, so 011 §10 G1 / 012 §8 G1 remain unmet. Don't reopen
  "should AgentEngine bind a port" without a new ADR; `decisions/README.md`'s ADR-061 row and the
  milestone-7 breakdown doc's Phase H have the full rationale and the real security defects this
  decision's red-teaming found and fixed in already-shipped code.
- **The licence is MIT, decided.** See [`LICENSE`](LICENSE); resolved 2026-08-04
  (024 Q1 / OQ-11). Not an open question.

## Feature vs. safety balance

**Default to enabling, not blocking.** `ADR-070` (Judged) records a deliberate project-owner
trade: ship a broader feature surface even where it costs some engine-enforced safety, via a
disciplined **Delegated Decision Seam** — explicit host opt-in, fails closed/safe when unset,
narrows or decides among already-possessed authority only (never mints/widens it), host code never
model output, always audited. Ship first behind that seam with real ADR evidence and honestly
disclosed residuals (`ADR-071`'s native unsandboxed process-execution providers is the model);
harden later, as a follow-on ADR, not as a precondition to shipping. This is not a license to
relax I2/I3 themselves — `ADR-070` §3 rejected that outright — it only bounds where responsibility
for an already-scoped risk can be delegated to the consumer dev, and it still needs its own ADR,
not a blanket exemption because the goal sounds reasonable.

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

**Sandbox and hostile tests are resource-capped** — a test proving a fork bomb is contained must not
be able to take the machine with it.

## Git

**`origin` is a real remote** — <https://github.com/thnak/AgentEngine> — so `git push` has a target
and pushing is a genuine, outward-facing action. This line previously read "Local repository, no
remote", which stopped being true once the project was published; a session that trusts it will
reach for the wrong default when asked to push. **Commit messages carry no co-author trailer**
(explicit project owner instruction).
