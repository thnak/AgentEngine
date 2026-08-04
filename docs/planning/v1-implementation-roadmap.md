# v1 implementation roadmap: milestones and sequencing

**Status:** Planning input, not a spec. This does not amend any RFC or change any promotion gate —
it proposes an order to build and prove them in. Every RFC stays **Draft** until its own §Promotion
gate (024 §4: Draft → Reviewed → Proven → Accepted) is actually run against real code; nothing here
changes that bar or substitutes for it. Reorder freely as implementation reveals what's actually
hard — this is a starting sequence, not a commitment.

## Why this order

The RFC dependency graph (`Depends on:` line in each doc) is **not a DAG** — it has real cycles
(018↔011↔012 over credentials and auth, with 004 depending one-directionally into that cluster;
006↔007↔008↔009 over tools, capabilities, sandboxing, and plugins all referencing each other's
concepts). That's expected for a system where the RFCs
describe facets of one integrated core rather than strictly layered modules, and it means a literal
topological build order doesn't exist. What follows instead is a **walking-skeleton sequence**:
each milestone is the smallest next capability that's actually demonstrable end-to-end, built by
implementing a cluster of mutually-referential RFCs together and stubbing whatever a later milestone
owns.

**Two disciplines run continuously from Milestone 0, not as their own milestone:**

- **027 (Vocabulary and Naming)** is normative for every public identifier from the first line of
  code — its G1 lint (every exported type appears in 027's tables) belongs in CI from day one, not
  bolted on later. Retrofitting naming discipline onto an existing surface is far more expensive than
  holding the line from the start.
- **024 (Versioning, Governance)** is largely already in force: `LICENSE`, `SECURITY.md`, and the
  ADR/promotion process exist now (OQ-11, 2026-08-04). New public surface follows §5's RFC-hygiene
  rules as it's written, not as a late cleanup pass.

**Machine safety applies to every milestone's build/test/bench work**: `-j4` max (`-j1` for TSan),
pinned cores for tests/benchmarks, hostile tests resource-capped — CLAUDE.md's standing rule, not
repeated per milestone below.

## Milestone 0 — Bootstrap

Not an RFC. Repo scaffolding: CMake ≥ 3.28 / C++23 per 021 §5, Quark pinned as a submodule (never
forked — CLAUDE.md's locked decision), a CI skeleton on Windows + Linux (021 §5's matrix, even if
most jobs are placeholders initially), and the 027 naming-lint stub wired in from the start.

**Exit:** an empty engine target links against the pinned Quark commit and builds clean on both
platforms under the CI matrix.

## Milestone 1 — Core substrate: the walking skeleton

**RFCs:** 001 (Execution Model), 003 (Message and Content Model), 007 (Capability and Trust Model —
core types only: `Capability`, `Principal`, `EffectContext`, the taint mechanism; the full policy
engine and sandbox enforcement land in M2).

001 and 003 have no internal RFC dependencies (Quark only) and nearly everything else depends on one
or both — they're the actual foundation regardless of what the dependency lines say elsewhere. 007's
*types* (not its policy evaluation or sandbox enforcement, which need 006/008/009 to mean anything)
are pulled forward here because I2/I3 need to be structurally true from the first agent that runs,
not retrofitted once effects exist.

**Exit:** a single hard-coded agent runs one turn against a mock `ChatClient`, on a Quark
`AgentSession` actor, with `Message`/`Content` as the wire shape and `Tainted<T>` compiling correctly
(007 §9 G2's compile-fail proof, provable even with nothing else built) — no tools, no sandbox, no
real provider, in-memory session only. This is 001 §9 G1/G2 in miniature.

This mock `ChatClient` (and the `ChatClientId` policy tag 002's CRTP surface requires at compile time,
needed once M2 authors an agent with `002`) is hand-rolled ahead of RFC 004's own design — 004 isn't
scheduled until M5 — rather than pulled forward as its own types-only slice. It gets reconciled
against the real seam when M5 builds `ChatClient` for real; expect the mock's shape to change then.

## Milestone 2 — Tools, capabilities enforced, sandboxed

**RFCs:** 006 (Tool and Function Plane), 008 (Sandbox and Isolation — `native-jail` first, per the
locked "native-jail first, the backend seam absorbs the rest" priority, §1), 009 (Plugin and
Extension System — the WASM Component Model ABI), 002 (Agent Model and Authoring — the CRTP surface,
now that there's something to author).

This is the cluster with the real cycle (006↔007↔008↔009). Build order within it: `Capability`
enforcement plumbing (007 §3's empty-by-default, attenuation-only rules) → the tool pipeline (006 §3)
with a trivial native tool → `native-jail` sandbox (008, reusing the ADR-004 spike's AppContainer +
Job Object design and measurements — **Status: Spiked, not Judged** — a red-team pass is still owed
before this backend can be cited as settling 008 §3, and the already-**Judged** caller-aware import
story, `decisions/ADR-003`, rather than re-deriving them) → the WASM plugin host (009) once a sandbox
exists to run it in.

**ADR-track items surfacing here, per CLAUDE.md's design→red-team→prove→judge discipline** (these
were resolved by reasoning in this session's spec pass, not proven against real code — flagged
explicitly rather than treated as settled):
- The first-party egress proxy (008 §10 Q3's resolution) — security-critical, host-mediated egress
  for every profile depends on it being right.
- The policy-reachability tool (007 §9 G6) — new CI tooling, not yet built.

**Exit:** an agent declares `Tools<...>`, a capability-gated native tool call is enforced end to end,
`native-jail` sandbox parity (008 §9 G1) holds on Windows and Linux, and one WASM `ae:tool` component
loads and executes (009 §10 G1).

## Milestone 3 — Code interpreter, CodeAct, worktree

**RFCs:** 025 (Worktree and Virtual Filesystem), 010 (Python Code Interpreter and Shell), 026
(Agent-Facing Runtime Surface — the `agent.*` library and the transparent-environment principle).

Worktree first (it's what makes the interpreter's state feel persistent, 010 §4), then the
interpreter/shell sharing one `ExecState` (010 §3a), then the `agent.*` library on top.

**ADR-track item:** `ShellRunner`'s grammar-parser fuzzing gate (010 §9 G8) — new fuzzing
infrastructure, build it here rather than deferring hostile-input hardening.

**Exit:** CodeAct runs a multi-step Python program against real tools and the worktree; 026 §8 G4
(transparency is not security — full hostile suite re-run against an agent explicitly told it's
sandboxed, results identical) passes, which is this RFC's central, falsifiable claim.

## Milestone 4 — Sessions, durability, memory

**RFCs:** 005 (Sessions, State and Memory), 019 (Durability and Long-Running Agents), 029 (Memory
System).

**Exit:** a session survives process restart with an identical resumed run (001 §9 G2 / 019 §7 G1),
`Suspended` holds zero resources (019 §7 G3), and the default (non-vector) memory retrieval path is
deterministic and replayable (029 §9 G1).

## Milestone 5 — Real providers, identity and secrets

**RFCs:** 004 (ChatClient Plane), 018 (Identity, Authorization and Secrets).

Deliberately after M1–M4, not before: everything up to here ran on a mock `ChatClient`, which is
sufficient for proving the engine's own mechanics without a live network dependency in the loop. Real
backends (OpenAI-compatible, Anthropic) and the secret seam (018 §4) come in once there's a full
agent worth pointing at them.

**Exit:** the same agent runs unchanged across ≥ 3 backends with only capability-table-predicted
differences (004 §7 G1), and secrets never appear in any persisted artifact under a canary scan
(018 §7 G2).

## Milestone 6 — Multi-agent orchestration

**RFCs:** 014 (Workflow and Orchestration), 030 (Project: Workspace Grouping and Directed Lifecycle).

**ADR-track item:** the worktree merge-drafting/confirmation split (025 §9 G7) becomes load-bearing
once concurrent agents on `shared`/`branch` sub-worktrees are actually exercised, not just specified —
worth a dedicated review pass here even though the design was resolved earlier.

**Exit:** each 014 §3 pattern runs correctly under injected executor failure (014 §8 G1), and pausing
one Project has zero observable effect on N-1 others (030 §7 G1).

## Milestone 7 — Protocol conformance

**RFCs:** 011 (MCP Conformance), 012 (A2A Conformance), 013 (UI and Streaming Surfaces — AG-UI, SSE,
OpenAI-compatible), 015 (Declarative Agent and Workflow Format).

This is where the project's own conformance-percentage discipline (011 §10) becomes real — the first
milestone where "we implement protocol X" is a published number, not a claim.

**Exit:** `conformance server`/`conformance client` pass at `2026-07-28` (011 §10 G1/G2), `a2a-tck`
passes with zero MUST-level failures (012 §8 G1), the AG-UI compatibility suite passes against a
pinned schema (013 §6 G5), and a YAML agent produces byte-identical metadata to its C++ equivalent
(015 §7 G1, I6's actual enforcement).

## Milestone 8 — Safety, observability, testing infrastructure, performance

**RFCs:** 017 (Safety and Content Governance), 016 (Observability), 022 (Testing and Evaluation —
the deterministic-simulation and golden-trace infrastructure, which should have been *used*
incrementally since M1 even though it's formalized here), 023 (Performance Targets and Budgets — this
is where every `TBD-baselined` cell in 023 §3 gets a real number on the reference machine).

**Exit:** the injection corpus (017 §8 G1) fails to produce any effect outside the run's capability
set on every profile; one connected trace crosses a cross-process agent call (016 §7 G2); every 023
§3 budget cell is baselined (023 §7 G1) with the methodology fixes from this session's resolutions
(measured-floor deltas for sandbox cold start, recorded-real-provider fixtures for streaming) applied
from the first run, not retrofitted.

## Milestone 9 — Hosting, platform hardening, bulk data

**RFCs:** 020 (Configuration and Hosting — all five shapes), 021 (Platform Support and Portability —
closing out the full matrix, including the arm64 re-baselining and the Windows/Linux `native-jail`
CPU-time comparison that stayed genuinely open this session pending a Linux backend to compare
against), 028 (Bulk Data Transfer and Zero-Copy — parallelizable with M4 onward once 026 exists, but
called out here as the point it's expected to be complete), plus the `remote` sandbox profile
(deferred from M2 — this is where `RemoteExecToken`, 008 §4a, actually gets exercised and owes the
forged/replayed/expired-token corpus and registry-lookup measurement its own resolution flagged as
outstanding).

**Exit:** the same agent runs unchanged in all five hosting shapes (020 §7 G2); the `remote` profile's
callback authentication is proven against a real negative corpus, not merely designed; every 023
budget is re-baselined on both platforms with divergences explained (023 §7 G4).

## v1 Promotion

Every RFC's own gate has run against real code (not this session's design-level resolution) and its
status moves Draft → Reviewed → Proven → Accepted (024 §4). The README's support table is generated
from CI results (021 §2's honesty requirement), not hand-written. This is the actual finish line —
everything above is sequencing toward it, not a substitute for it.

## What this doc is not

- **Not a schedule.** No dates, no sprint-length commitment — team size and unknowns will change the
  pacing far more than anything guessable now.
- **Not a promise that the cycles resolve cleanly.** The 006↔007↔008↔009 and 018↔011↔012 clusters
  (the latter with 004 depending one-directionally in) may need real back-and-forth once
  implementation exposes what the specs got wrong — that's what the gates are for.
- **Not exhaustive on ADR-track items.** Only the ones surfaced by this session's resolution pass are
  flagged; implementation will surface more contested/security-critical decisions that weren't
  visible from spec review alone.
