# ADR-051 — The 014 §8 G3 scheduling-shuffle test, built for the first time

**Status:** Proposed (2026-08-14). Designed, self-red-teamed, implemented, and proven (real code +
new test file, full suite green); awaiting the project owner's explicit "Judged" sign-off per this
project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #22 (the finding this
ADR closes). `014-Workflow-and-Orchestration.md` §8 G3 (the gate itself: "shuffling intra-round
executor scheduling across 10³ seeds produces identical workflow output; an intentionally order-
dependent executor is detected by the same test"). `docs/planning/milestone-6-multi-agent-
orchestration-breakdown.md` decisions 5/8 (the already-designed architecture this ADR builds
against, unchanged) and its own completion section (corrected in the same pass, §2 below).

## 1. The question

**Stated so it has a wrong answer:** does 014 §8 G3's own scheduling-shuffle test exist anywhere in
this codebase?

**Before this fix: no.** Confirmed directly — no test file under `tests/` matched "shuffle" in the
workflow-scheduling sense before this one (`test_worktree_branch_concurrency.cpp`'s own "shuffle"
hit is an unrelated worktree-branching concept). The M6 breakdown doc's own decisions 5 and 8 only
ever DESIGNED this test ("the shuffle test's seed loop is the one place this milestone could
accidentally saturate the box... Phase J must assert the spread as a precondition"); nothing in this
codebase ever claims to have built it.

## 2. What re-grounding against current code AND docs found

- **The gap audit's own "'the test exists' falsely claimed in more than the two spots" framing
  describes the AUDIT's own internal architect/red-team exchange while proposing a fix for this gap
  — not a claim that shipped anywhere in this repository.** Searched every markdown file in the tree
  for a claim that G3 was built/passing; found none. What WAS found, independently, is a real,
  narrower documentation gap worth fixing in the same pass (§2's next bullet).
- **The M6 breakdown doc's own Phase J section silently dropped G3 between "delivered" and
  "explicitly deferred."** Its decision 8 explicitly named G3 as something "Phase J must assert... as
  a precondition," but Phase J's own completion summary ("Milestone 6 is complete (Phases A-J)")
  names only 014 §8 G1 and 030 §7 G1 as "the pair Phase J proves" — G3 appears in neither that claim
  NOR the doc's own separate "what Milestone 6 deliberately does NOT close" list. It fell through the
  gap between the two lists rather than landing honestly in either. Corrected in the same pass as
  this ADR (a new "Update (2026-08-14...)" note in that doc, matching this project's own established
  correction convention rather than silently rewriting history).
- **The CURRENT fan-out mechanism (post-ADR-037) is `rt::ThreadPool`-backed, not Quark's
  `AskFuture<R>`** (the mechanism the M6 breakdown doc's decision 5 was originally written against,
  before ADR-037 removed Quark). Re-verified directly against `rt/workflow_supervisor.hpp`'s own file
  banner: fan-out issues every round item as an independently-submitted `ThreadPool` job BEFORE
  collecting any, then collects via `std::future<JobOutcome>::get()` in FIXED INDEX ORDER — the
  identical "issue-all-then-collect-in-declared-order" property decision 5 designed this test
  against, just running on the new substrate. This test is built and proven against the REAL, current
  mechanism, not the historical (now-deleted) Quark one the original design prose describes.

## 3. The design

`tests/test_rt_workflow_supervisor_scheduling_shuffle.cpp` (new), reusing `test_rt_workflow_
supervisor_patterns.cpp`'s own FI-1/FI-2 graph shape (a `src` node fanning out to four parallel
branches, fanning back into one `agg` node) — that file already proved a NARROWER version of this
exact claim (fixed source-order merge, for one hand-picked delay assignment); this ADR broadens it to
a real 10³-seed sweep with genuinely random per-branch delays, so real executor completion order
actually varies from run to run, not just in principle.

**G3-POS (the positive claim):** ONE `WorkflowSupervisor` (one `ThreadPool`) is built once and
re-run sequentially across 1000 seeds — `run_workflow()`'s own full state reset per call
(`state_`/`ports_`/`rounds_` cleared, `run_counter_` incremented) makes this safe, matching decision
8's own explicit "the failure mode to avoid is spawning an Engine per seed; the test builds one
Engine and re-runs the graph." Each seed assigns a fresh `std::mt19937`-derived random microsecond
delay (0-3ms) to each of the four branches before that run; the assembled output is compared against
seed 0's own reference output. All 1000 must match byte-for-byte.

**G3-NEG (the gate's other half, same test):** a second, separately-bodied graph of the identical
shape, where each branch bakes its own REAL completion position (a shared `std::atomic<int>` counter,
reset once per run) into its returned text — a concrete instance of "an executor whose output depends
on intra-round ordering" (014 §8's own wording). Run across a 200-seed subset of the same delay
sweep; at least one seed must produce output different from seed 0's. This is what proves G3-POS
isn't vacuous — the SAME randomized-delay mechanism that produced zero divergence against the correct
graph is shown, on the correct (broken) graph, to be capable of producing divergence at all.

## 4. Self-red-team findings

**A pure "collect-in-fixed-order" claim needed a REAL divergent-output negative control, not a
restatement of the mechanism.** An earlier framing considered simply re-asserting FI-2's own
already-proven "merge order is source order" claim at 1000x scale — but that alone cannot distinguish
"the invariant holds" from "this test would pass no matter what the graph does," since a
non-order-dependent graph passes trivially regardless of how much delay-randomization surrounds it.
The negative control (an executor that DOES leak completion order) is what makes G3-POS's clean
result meaningful rather than assumed.

**The negative control's own sweep size was deliberately NOT also 1000.** Its only job is to prove
real divergence is reachable at all — a much smaller independent sample (200) already makes that
overwhelmingly likely given four branches with independently randomized delays, at a fraction of the
wall-clock cost of a second full 1000-seed run. 014 §8 G3's own literal "10³ seeds" language is
about the POSITIVE claim specifically; the negative control exists to validate the test's own
sensitivity, not to independently re-satisfy that count.

**Checked, not assumed: the current (post-ADR-037) fan-out mechanism still has the property this
test needs.** Re-read `workflow_supervisor.hpp`'s own file banner directly rather than trusting the
M6 breakdown doc's Quark-era description — confirmed the `ThreadPool`-backed replacement preserves
"issue all, collect in fixed index order" bit-for-bit, so this test proves a claim about the REAL
current substrate, not a claim about deleted code.

## 5. What this ADR does not claim

- **Not a claim that every 014 §3 pattern is individually shuffle-tested** — this proves the
  GENERAL mechanism (fan-out/fan-in round assembly) via one representative graph shape, matching
  `test_rt_workflow_supervisor_patterns.cpp`'s own precedent of proving routing kinds against
  representative shapes rather than every possible graph topology.
- **Does not touch `WorkflowSupervisor::pool_`'s own uncapped `hardware_concurrency()` default worker
  count** — pre-existing, already used identically by the already-passing FI-1/FI-2 tests this file
  reuses the shape of; out of scope for this ADR, named rather than silently inherited without
  comment.
- **The ~19-second real wall-clock cost of this test is a known, accepted trade**, not an oversight —
  matching the M6 breakdown doc's own decision 8, which explicitly anticipated the seed loop as "the
  one place this milestone could accidentally saturate the box" and scoped the design (one Engine,
  sequential re-run, bounded per-branch delay) specifically to keep that cost real but bounded, never
  CPU-saturating (the wall time is overwhelmingly sleep-blocked, not compute-bound).

## 6. Evidence

`tests/test_rt_workflow_supervisor_scheduling_shuffle.cpp`: G3-POS (1000 seeds, byte-identical
output every time) and G3-NEG (200-seed subset, a deliberately order-dependent executor's output
provably diverges) both pass. `docs/planning/milestone-6-multi-agent-orchestration-breakdown.md`
corrected in the same pass to honestly record G3's prior silent omission and this closure.

Full suite: green (`ctest`, this pass), zero regressions.
