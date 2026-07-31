# 022 — Testing and Evaluation

**Status:** Draft · **Depends on:** 001, 008, 016, 019, Quark 014 · **Gate:** §7

## Goal

Make agent behaviour testable the way a compiler is testable: deterministic, reproducible, and
regression-gated — plus the *evaluation* layer that non-deterministic model behaviour actually
requires.

## 1. Test kinds

| Kind | What it covers | Determinism |
|---|---|---|
| **Unit** | Types, schemas, mappings, policy evaluation | Exact |
| **Simulation** | Full engine on Quark's deterministic scheduler: interleavings, failures, timeouts, restarts, with no real threads or clocks | Exact |
| **Replay** | A recorded run re-executed offline (001 §7) | Exact |
| **Conformance** | Protocol suites at a named revision (011, 012, 013) | Exact |
| **Hostile** | Sandbox containment, injection, path escape, capability leaks (008, 017) | Exact outcome class |
| **Integration** | Real providers, real MCP servers, real sandboxes | Non-deterministic; quarantined from the gate |
| **Evaluation** | Task success, quality, cost, safety over a dataset | Statistical (§4) |

**Rule:** the CI gate consists of the deterministic kinds. Integration and evaluation run on a
schedule and report trends; a flaky live-provider test must never be able to block a merge, or the
gate gets disabled by the third person it inconveniences.

## 2. Deterministic simulation

Built on Quark 014's simulation scheduler:

- **Virtual time** — timeouts, deadlines, reminders, and idle passivation are tested in
  milliseconds of real time.
- **Controlled interleavings** — the scheduler explores orderings; a seed reproduces a failure
  exactly.
- **Fault injection** — provider errors and partial streams, tool failures, sandbox crashes/OOM/
  timeouts, store failures, node loss, network partitions, checkpoint corruption.
- **Mock provider** — scripted responses including tool calls, streaming chunk boundaries,
  malformed output, and schema violations. Chunk boundaries matter: streaming-dependent bugs hide
  between them.

## 3. Golden traces

A test case is `{scripted inputs, recorded nondeterminism} → expected event stream`. Comparison is
over the **event stream** (013 §1), not over free text — so it is stable against wording changes
while catching behavioural ones (which tool, which arguments, which decisions, which order).

Golden traces are the regression net for prompt changes, provider upgrades, and refactors. They are
reviewed as diffs, because an unreviewed golden-trace update is a rubber stamp.

## 4. Evaluation

Models are non-deterministic; the answer is statistics with pre-registered thresholds, not asserting
on one sample.

- **Datasets**: task suites with inputs, environment fixtures (mock tools, seeded workspaces), and
  graders.
- **Graders**: exact/schema match, programmatic checks, rubric-based model grading (with the grader
  model pinned and versioned), and human review sampling.
- **Metrics**: task success rate, tool-selection accuracy, turns to completion, token cost, latency,
  safety violations, and **variance** — a high-variance agent is a broken agent even at a good mean.
- **Comparison** across agent versions, prompts, models, and configurations, with confidence
  intervals and a pre-registered decision rule. Post-hoc threshold selection is how a regression
  ships.
- Evaluation runs are ordinary engine runs (recorded, traced, auditable) so their evidence is the
  same evidence as production's.

## 5. Testing the security properties

Security claims get *positive controls* — a test that cannot fail proves nothing:

- Every containment test is paired with a control that **deliberately disables** the control and
  observes the failure it prevents (008 §9 G2, 017 §8 G3, 020 §7 G1).
- Capability leak tests fuzz derivation chains for a widening (007 §9 G3).
- Canary scanning for secrets and prompt content across every persisted artifact (016 §7 G3,
  018 §7 G2).
- The injection corpus grows from real findings and is versioned with the engine.

## 6. Test infrastructure

- **`TestKit`** — build an engine with mock providers, in-memory stores, a fake clock, and a
  recording sandbox; assert over event streams; drive approvals and input requests programmatically.
- **Fixtures as data** — scripted conversations, workspaces, plugin bundles, and MCP/A2A server
  doubles live in `tests/fixtures/`, not in code.
- **Reproduce from a seed**: any simulation failure prints a one-line command that reproduces it.

## 7. Promotion gate

- **G1** — the deterministic suite is green on all Supported platforms, under ASan/UBSan, and TSan
  on Linux.
- **G2** — a simulation failure reproduces exactly from its seed, 100/100 times.
- **G3** — every load-bearing invariant in specs 001–021 has a named test, verified by an
  invariant→test coverage map that fails CI when an invariant has none.
- **G4** — every hostile/security test has a passing positive control.
- **G5** — an evaluation run over a reference dataset produces stable metrics across repeats within
  a declared confidence interval.

## 8. Open questions

- **Q1** — Model-graded evaluation is itself non-deterministic and drifts as the grader model
  changes. Pinning helps; it does not solve comparability across grader versions.
- **Q2** — How much of the injection corpus can be published without handing out an attack kit.
- **Q3** — Whether golden traces should be normalized (timestamps, ids) automatically or authored
  normalized.
- **Q4** — Cost of running conformance suites against live third-party servers versus recorded
  doubles; doubles drift, live is flaky.
