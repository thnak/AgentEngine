# 023 — Performance Targets and Budgets

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** all · **Gate:** §7

## Goal

Turn every performance claim into a **pass/fail verdict a benchmark prints**, against a named
reference machine — the discipline Quark's 023 established, applied to agent-shaped work.

## 1. What is being measured

Agent latency is dominated by model inference, which the engine does not control. So the budgets
here measure **engine overhead** — the time and memory the engine adds around the parts it does not
own — plus the things it fully owns: sandbox lifecycle, session density, and streaming throughput.

**A budget expressed as end-to-end latency including a provider is not a budget; it is a weather
report.**

## 2. Budget classes

| Class | Meaning |
|---|---|
| **Hard** | Exceeding it fails the gate and blocks the merge |
| **Goal** | Exceeding it warns and requires a written justification |
| **Free** | Must be indistinguishable from a hand-written equivalent (zero-cost claims) |

## 3. Provisional budgets

**Provisional pending a reference-machine baseline** — the numbers below are targets derived from
the design, not measurements. They become real when §7 G1 runs. Every cell is `TBD-baselined`
until then.

| Area | Metric | Budget | Class |
|---|---|---|---|
| Turn overhead | Engine time per turn excluding provider and tools, p50 / p99 | ≤ 200 µs / ≤ 1 ms | Hard |
| Context assembly | Assemble a 50-message context, p99 | ≤ 500 µs | Goal |
| Streaming | Per-chunk engine overhead | ≤ 10 µs, **0 allocations** | Hard |
| Streaming | Sustained chunk throughput per session | ≥ 100 k/s | Goal |
| Tool dispatch | Native tool, engine overhead p50 / p99 | ≤ 20 µs / ≤ 100 µs | Hard |
| Plugin call | WASM warm invocation, trivial function, p99 | ≤ 100 µs | Goal |
| Plugin instantiate | Pooled instance acquire, p99 | ≤ 1 ms | Goal |
| Sandbox `wasm` | create+exec+destroy, trivial, p50 / p99 | ≤ 5 ms / ≤ 20 ms | Goal |
| Sandbox `native-jail` | cold create+exec, p50 | ≤ 200 ms | Goal |
| Session | Activation from store, p50 / p99 | ≤ 1 ms / ≤ 10 ms | Goal |
| Session | Idle sessions per GB (10-message history) | ≥ 100 k/GB | Goal |
| Checkpoint | Turn-boundary checkpoint write, p99 | ≤ 5 ms | Goal |
| Policy | Capability + policy evaluation per effect, p99 | ≤ 10 µs | Hard |
| Observability | Overhead with tracing at 100 % | ≤ 5 % of engine time | Goal |
| Zero-cost | All-default agent vs hand-written loop | indistinguishable | Free |
| Zero-cost | Absent middleware / absent hooks | no added branch or indirect call | Free |
| Scale | Concurrent active runs per node (mock provider) | ≥ 10 k | Goal |
| Scale | Concurrent sandboxes per node (`wasm`) | ≥ 1 k | Goal |

## 4. Machine-independent invariants

These are **pass/fail CTest gates**, not noise-sensitive benchmarks, and they are the ones that
matter most because they cannot be excused by hardware:

- **0 heap allocations** in the per-chunk streaming path.
- **0 cross-core atomic RMW** added to Quark's drain path by AgentEngine.
- **A run's steady state is one Quark activation** — not a thread, not a fiber.
- **A suspended run holds zero activations, sandboxes, connections, and threads** (019 §7 G3).
- **Capability handles do not outlive their invocation** (007 §9 G4).
- **No sandbox, process, file, or handle leak** over 10⁵ cycles (008 §9 G4).

## 5. Reference machine and method

- A named reference machine, recorded in `PERFORMANCE.md` with CPU, memory, OS, and compiler
  versions. Absolute numbers without it are meaningless.
- **Percentiles, never means.** p50, p99, p999 where tail matters.
- Pinned cores per CONVENTIONS; the same caps that keep the dev box alive also keep numbers stable.
- Mock providers and fixtures so measurements isolate engine overhead.
- Every budget cell names the benchmark that produces it; a cell with no benchmark is not a budget.

## 6. Regression gate

`bench-gate` fails on a Hard miss, warns on a Goal regression, and fails on any Free-class
divergence. It runs on every push on the reference machine. Results and the reproduce steps live in
`PERFORMANCE.md`; correctness results in `VERIFICATION.md` (both created when the numbers exist —
this project does not ship an empty results file to look organized).

## 7. Promotion gate

- **G1** — every Hard- and Goal-class §3 cell has a benchmark and a baselined number on the
  reference machine, replacing the provisional target with a measurement.
- **G1b** — every Free-class §3 cell is verified by a passing pass/fail CTest gate, the same
  mechanism §4 already uses for its machine-independent invariants — a Free-class row is a
  structural/codegen check (indistinguishable from hand-written, no added branch or indirect call),
  not a numeric benchmark, so it has no baselined number to demand. This CTest gate runs on every
  build; that is what catches a Free-class row regressing — a zero-cost claim that silently gains a
  branch or allocation fails the next build, not just a scheduled benchmark run.
- **G2** — every §4 invariant has a passing CTest gate with a positive control.
- **G3** — the gate detects a deliberately introduced 20 % regression in each Hard cell.
- **G4** — budgets are re-baselined on a second platform (Windows and Linux), with divergences
  explained rather than averaged.

## 8. Open questions

- ~~**Q1** — Which reference machine. Quark's is a virtualized Xeon Silver 4208; sharing it makes
  cross-project comparison possible and understates modern hardware.~~ **Resolved, Quark's own
  reference machine (2026-08-04):** the quantity this RFC measures — engine *overhead*, what
  AgentEngine adds on top of Quark (§1) — is only meaningfully comparable against Quark's own
  baseline if measured on the identical machine; a different one would need cross-machine
  normalization for the one comparison that matters most to a project built as a layer over Quark.
  "Understates modern hardware" is a real but weaker cost here than for raw-throughput numbers:
  these are relative overhead budgets (avoided allocations, avoided atomic RMW, coroutine dispatch
  cost), which are less hardware-generation-sensitive than absolute compute throughput. A second,
  modern-hardware baseline can be added later as an informational addendum in `PERFORMANCE.md`
  without changing which machine gates the merge, extending §7 G4's own "re-baselined on a second
  platform, divergences explained rather than averaged" pattern from platform to hardware generation.
- ~~**Q2** — Whether sandbox cold-start budgets should be per-profile absolutes (as written) or
  ratios against a measured floor, given they are dominated by the OS and hypervisor.~~ **Resolved,
  a measured-floor delta, matching this RFC's own §1 principle applied consistently (2026-08-04):**
  §3's absolute `native-jail` cold-start number was actually an inconsistency with §1's own stated
  philosophy — measure what the *engine* adds, not what the underlying platform costs — since a
  number dominated by AppContainer/Job-Object creation is mostly measuring Windows' own baseline,
  which can drift for reasons entirely outside AgentEngine's code (an OS update), producing false
  regression alarms under §6's gate discipline. The gate measures **AgentEngine's added overhead**:
  `(AgentEngine sandbox create+exec+destroy) − (a bare OS-level equivalent measured on the same
  machine at gate time, e.g. a minimal AppContainer/namespace+cgroup creation with no engine code
  involved)`, as a delta or ratio once §7 G1's real measurement determines which is more stable —
  isolating engine regressions from platform noise the way every other budget in §3 already does.
  This is a methodology fix, not a number change; §3's cells were already `TBD-baselined`.
- ~~**Q3** — Token-throughput budgets require a provider model; a mock cannot represent real
  streaming cadence, and a real one is not reproducible.~~ **Resolved — neither; a recorded-and-
  replayed real stream, which this project already has the machinery for (2026-08-04):** 004 §7 G3
  already requires "a recorded streamed run replays offline with identical chunk boundaries" — the
  exact tool this question needs. Capture a real provider's actual streaming cadence once (001 §7's
  recording mechanism), then replay it deterministically for every benchmark run: realistic (real
  chunk timing from an actual provider, not a guessed synthetic cadence) and reproducible (replay is
  deterministic, satisfying §5's fixture-based isolation). §5's "mock providers and fixtures" line is
  clarified to mean recorded-real-provider fixtures specifically for the chunk-cadence-sensitive
  budgets — a synthetic mock stays fine for the rest of §3's table, which doesn't depend on cadence.
  Recordings should be refreshed periodically as providers change their own infrastructure — an
  operational-freshness responsibility, the same pattern already applied to the interpreter image
  (010 §10 Q1) and pricing tables (016 §8 Q2), not a new mechanism.
