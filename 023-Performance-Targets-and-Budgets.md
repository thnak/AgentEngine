# 023 — Performance Targets and Budgets

**Status:** Draft · **Depends on:** all · **Gate:** §7

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

- **G1** — every §3 cell has a benchmark and a baselined number on the reference machine, replacing
  the provisional target with a measurement.
- **G2** — every §4 invariant has a passing CTest gate with a positive control.
- **G3** — the gate detects a deliberately introduced 20 % regression in each Hard cell.
- **G4** — budgets are re-baselined on a second platform (Windows and Linux), with divergences
  explained rather than averaged.

## 8. Open questions

- **Q1** — Which reference machine. Quark's is a virtualized Xeon Silver 4208; sharing it makes
  cross-project comparison possible and understates modern hardware.
- **Q2** — Whether sandbox cold-start budgets should be per-profile absolutes (as written) or
  ratios against a measured floor, given they are dominated by the OS and hypervisor.
- **Q3** — Token-throughput budgets require a provider model; a mock cannot represent real
  streaming cadence, and a real one is not reproducible.
