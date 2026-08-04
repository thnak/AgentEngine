# 022 — Testing and Evaluation

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 008, 016, 019, Quark 014 · **Gate:** §7

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
- The injection corpus grows from real findings and is versioned with the engine. **Cadence (OQ-10,
  2026-08-04):** every 024 §4.2 design→red-team→prove→judge cycle touching 007/008/017/018 already
  produces adversarial attempts as part of judging its ADR; those attempts become corpus entries by
  default when the ADR is judged, not as separate follow-up work someone has to remember — the corpus
  grows exactly as often as security-relevant design work happens. Backstop: a quarterly check
  confirms every ADR judged that quarter contributed its findings, and triggers a manual review pass
  if a quarter passes with no security-relevant ADR activity at all, so the corpus can't go stale
  purely from a quiet quarter.

## 6. Test infrastructure

- **`TestKit`** — build an engine with mock providers, in-memory stores, a fake clock, and a
  recording sandbox; assert over event streams; drive approvals and input requests programmatically.
- **Fixtures as data** — scripted conversations, workspaces, plugin bundles, and MCP/A2A server
  doubles live in `tests/fixtures/`, not in code.
- **Reproduce from a seed**: any simulation failure prints a one-line command that reproduces it.

## 7. Promotion gate

**Invariant→test coverage map.** The map is generated and checked, not hand-maintained. I1–I8 from
the specification, plus any RFC-local invariant, carry a stable identifier written where the
invariant is introduced in that RFC's prose (e.g. `I3`, `020-INV-2`). A test declares which
identifier(s) it exercises using that same tag (e.g. a `covers: I3, 020-INV-2` annotation the test
runner can read). A CI linter, independent of both the RFCs and the test corpus, cross-references
every invariant tag it finds in Accepted-RFC prose against every tag declared by the test corpus:
an invariant with no matching test fails CI, and a test tag that resolves to no real invariant fails
CI too. This is what proves the map's enumeration is complete — the map is a diff of two
independently-scraped tag sets, not a document someone asserts is up to date.

- **G1** — the deterministic suite is green on all Supported platforms, under ASan/UBSan, and TSan
  on Linux.
- **G2** — a simulation failure reproduces exactly from its seed, 100/100 times.
- **G3** — every load-bearing invariant across every Accepted RFC has a named test, verified by the
  invariant→test coverage map above.
- **G4** — every hostile/security test has a passing positive control.
- **G5** — an evaluation run over a reference dataset produces stable metrics across repeats within
  a declared confidence interval.

## 8. Open questions

- ~~**Q1** — Model-graded evaluation is itself non-deterministic and drifts as the grader model
  changes. Pinning helps; it does not solve comparability across grader versions.~~ **Resolved: never
  compare across grader versions directly — re-baseline (2026-08-04):** the same re-baselining
  discipline already used for performance budgets (023 §7 G4) and every other pinned-dependency bump
  this session. §4's grader pin gives reproducibility *within* a version; when the grader bumps
  (a deliberate, versioned change), retained historical comparison points are re-scored under the new
  grader in the same pass, rather than their old scores being reconciled against new ones
  mathematically — converting an unsolved cross-version statistical problem into "always compare
  same-grader-version." The cost is re-running old fixtures, cheap relative to a comparison nobody
  could actually vouch for otherwise.
- ~~**Q2** — How much of the injection corpus can be published without handing out an attack kit.~~
  **Resolved, classification public, payloads private (OQ-10, 2026-08-04):** publishable —
  categories, counts, and which gate each entry maps to (017 §8 G1–G5) — generated from CI the same
  way 021 §2's support table is (021's honesty requirement: a tier is a statement about what CI
  proves, not a hand-written claim), so the published record can't drift from what was actually run.
  Private — the attack strings/payloads themselves, until the fix proven effective against them ships
  (i.e., until the relevant gate passes with that entry included), for the same reason a CVE's initial
  disclosure states impact and classification before a full PoC: publishing exploit strings ahead of
  a proven fix hands out an attack kit for no defensive benefit. Owner: the same authority that judges
  an ADR (024 §7 Q3, resolved as the project owner while solo, OQ-11). This rule is written into
  `SECURITY.md` (024 §7 Q4, OQ-11), not tracked as a separate policy.
- ~~**Q3** — Whether golden traces should be normalized (timestamps, ids) automatically or authored
  normalized.~~ **Resolved, authored-normalized (2026-08-04):** the better fit for §3's own "reviewed
  as diffs, an unreviewed update is a rubber stamp" discipline — a reviewer looks at an ordinary git
  diff of the fixture file directly, and that diff needs to be clean of timestamp/id noise for the
  review to mean anything. Tool-side normalization keeps the comparison *logic* clean but leaves the
  fixture *file* noisy on every ordinary re-recording — exactly the review-noise problem this RFC
  already worries about. A recording/generation tool substitutes canonical placeholders
  (`<TIMESTAMP>`, `<RUN_ID>`, etc.) when a fixture is captured, so both the raw file and the
  comparison are noise-free by the same mechanism.
- ~~**Q4** — Cost of running conformance suites against live third-party servers versus recorded
  doubles; doubles drift, live is flaky.~~ **Resolved — dissolves once conformance and integration
  stay the two different things §1 already defines them as (2026-08-04):** conformance suites
  (`@modelcontextprotocol/conformance`, `a2a-tck`) already test against the suite's *own* reference
  fixtures — the deterministic double the question was weighing against live servers, except
  maintained upstream by the protocol authors rather than built and left to drift by this project
  (011 §10's own example commands already show this: `conformance server --url
  http://localhost:3000/mcp` tests our implementation against the suite's tooling, not a third
  party's live deployment). That is what stays in the CI gate (§1: "Conformance... Exact... the CI
  gate consists of the deterministic kinds"). Testing against real, in-the-wild third-party MCP
  servers/A2A peers is squarely the Integration row — already non-deterministic and quarantined from
  the gate, run on a schedule, reporting trends. No new tradeoff needed; the question conflated two
  rows of a table this RFC already has.
