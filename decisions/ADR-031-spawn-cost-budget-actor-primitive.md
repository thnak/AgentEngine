# ADR-031 — `SpawnCostBudgetActor`: the actor-serialized spawn-cost pool primitive

**Status:** Judged (2026-08-14, project owner sign-off). Designed (inherited from `026-Agent-
Facing-Runtime-Surface.md` §9 Q1's own already-red-teamed sketch — see below), implemented, and
proven (real code + deterministic + real-concurrency tests, §4).

**Update (2026-08-14, at sign-off review — behavioral claims unaffected, code location corrected):**
`decisions/ADR-037-remove-quark-as-core-runtime.md` (executed 2026-08-13, three days after this ADR
landed) removed Quark entirely — the `quark::Actor<SpawnCostBudgetActor, quark::Sequential>` type
§2/§6 describe below no longer exists. It was ported to `include/agentengine/rt/spawn_cost_
budget.hpp` as `agentengine::rt::SpawnCostBudget`, re-expressed over `rt::AsyncMutex` instead of
Quark's `Sequential` dispatch — that file's own top comment states directly this is "unchanged
here, just re-expressed." Re-verified at sign-off: `tests/test_rt_spawn_cost_budget.cpp` (the
ported T1/T2 claims below) passes in full, including T2's real-concurrency double-spend proof. §2
and §6 below are kept as written (the historical record of what was actually designed/reviewed) —
read `Actor<..., Sequential>`/`trust/spawn_cost_budget.hpp` as historical, current code lives at
the path named in this note.

**Relates to:** `decisions/ADR-006-agent-spawn-depth-budget-bound.md` (the depth half of the same
"depth and budget bounds" question — this ADR is the cost half, deliberately kept a SEPARATE type
for the reason both this ADR and 026 §9 Q1 explain); `OpenQuestions.md` OQ-14 (amended by this
ADR); `decisions/ADR-030-session-scoped-codeact-wiring.md` (the most recent, independent
confirmation that no real `agent.spawn` call path exists anywhere in this codebase — this ADR does
not build one either, see §5).

## 1. The question

026 §9 Q1 already contains a full design → red-team cycle for a `SpawnCostBudget` type, written
2026-08-11 and explicitly marked "Draft design sketch... deliberately NOT an ADR — no code, no
tests, implementation currently paused." **This ADR's job is narrower than designing something
new: turn that already-red-teamed sketch into real, tested code**, per this project's own rule that
"a design without a falsifiable gate does not get written down as settled" (CLAUDE.md) — a
principle-only sketch, however carefully reasoned, is not itself evidence.

**Stated so it has a wrong answer:** does routing a consumable token pool's check-and-decrement
through a real Quark actor's own `Sequential` dispatch actually close the concurrent double-spend
026 §9 Q1 predicts a bare copyable value type would suffer — provable only by exercising real
concurrent dispatch (`quark::TestKit` cannot do this: it drains one ask fully before a second can
even be issued, so it structurally cannot produce the race under test)?

## 2. The design (inherited, not re-litigated)

026 §9 Q1's own reasoning, restated because it is why this is a SEPARATE type from
`SpawnBudget` rather than an added field on it: depth is a **ceiling** — ordinary for two
concurrent siblings to each independently compute `parent_depth − 1` off the same parent, both
correctly landing on the same answer, which is exactly why `SpawnBudget::attenuate_for_spawn()`
(ADR-006) is a pure, `const`, immutable-value-type call. Tokens are a **consumed pool**, not a
ceiling: two concurrent `agent.spawn` calls (concurrency this engine already supports —
`Parallelizable`, 006 §6b; `Concurrent`/map-reduce fan-out, 014 §3) each independently attenuating
a bare copyable `remaining_tokens_` snapshot could each succeed up to the full remaining amount — a
real double-spend that defeats the mechanism's entire purpose (007 §3 property 3: capability
handles materialize independently per invocation, so a naive copy-and-check pattern here inherits
the same hazard).

**Accepted mechanism**: `SpawnCostBudgetActor` (`include/agentengine/trust/spawn_cost_budget.hpp`)
— a real `quark::Actor<SpawnCostBudgetActor, quark::Sequential>` holding one `std::uint64_t
remaining_`. `initialize(total_tokens)` sets the pool once, configuration-time (no top-up/reset
method exists — a refillable pool would need its own authority question 026 §9 Q1 does not ask
for, and this ADR does not invent one as a drive-by). `handle(Ask<ConsumeSpawnTokens,
result<SpawnTokenGrant>>)` checks-and-decrements as ONE step inside the actor's own Sequential
handler — Quark's own dispatch guarantee (the same invariant every other stateful actor in this
codebase, including `AgentSession` itself, already relies on) means no second concurrent ask can
ever observe the pre-decrement value, closing the double-spend by construction, not by any lock
this file writes.

**One deliberate deviation from `AgentSession`'s own established idiom, explained rather than
silently copied**: `AgentSession`'s handlers fail closed by simply never calling `m.respond()`
(agent_session.hpp, throughout). That idiom is safe there only because every existing caller drives
it through `quark::TestKit`, which resolves an unanswered ask synchronously and immediately.
`reply_cell.hpp`'s own comment documents that under a REAL engine, a genuinely unanswered `Ask`
only resolves at actor teardown/reclaim — which would leave a live `block_on` caller parked
indefinitely for the ordinary "budget exhausted" case, since this ADR's whole point is to be
exercised under a real, running `quark::Engine`. `SpawnCostBudgetActor` therefore always responds,
using `result<SpawnTokenGrant>` as the reply type, sidestepping that hazard rather than inheriting
an idiom only ever verified safe under `TestKit`.

## 3. What this ADR deliberately does NOT do

Sourcing the amount a child receives is **not** this actor's job. 026 §9 Q1 requires that amount
come only from the target agent's own compiled `AgentMetadata::token_budget`, never from anything a
model's own output could set (I2/I3) — `SpawnCostBudgetActor` has no visibility into
`AgentMetadata` at all, by design; whichever future `agent.spawn` call path wires this in must read
that field itself before ever constructing a `ConsumeSpawnTokens{amount}`. Attribution (WHO is
spending) likewise travels through no field here — the same "audit is the caller's job, not the
primitive's" split `invoke_tool`'s own ten-step pipeline already establishes elsewhere.

Wall-clock/deadline budgeting stays fully, explicitly open (026 §9 Q1's own text, unchanged) — no
existing per-run deadline-enforcement mechanism exists anywhere in this codebase to attenuate
against yet.

## 4. Falsifiable claims and verdicts

`tests/test_spawn_cost_budget.cpp`, deterministic where it can be (T1) and genuinely
concurrency-dependent where the claim requires it (T2 — a real `quark::Engine` with 4 workers, not
`TestKit`). T2 was additionally re-run 5 times consecutively during this ADR's own prove phase
(not merely once) — all 5 runs identical, no flake observed.

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| T1 | Basic correctness: consuming within budget succeeds and decrements exactly; consuming beyond what remains is denied WITHOUT decrementing; the pool can reach exactly zero and then denies everything. | `test_spawn_cost_budget.cpp` T1: consume 30/100 → granted 30, remaining 70; consume 80 (only 70 left) → denied with `spawn_cost_budget.exhausted`, remaining STILL 70 (not partially decremented); consume the remaining 70 → granted 70, remaining 0; consume 1 more → denied. | **CORRECT** |
| T2 | Under REAL concurrent dispatch (not `TestKit`), the sum of every granted amount never exceeds the initial pool — no double-spend. | T2: 8 real `std::thread` callers, each `block_on`-ing a `ConsumeSpawnTokens{130}` ask against the SAME actor instance from a shared pool of 1000 (8×130=1040 > 1000, guaranteeing genuine contention, not a lucky everyone-fits run) — sum of grants ≤ 1000 in all 5 runs, at least one caller denied every run, and the actor's own final `remaining()` exactly equals `1000 − sum(granted)` (no lost or double-counted decrement). | **CORRECT** |

## 5. What this ADR does not claim

- **`agent.spawn` itself has no real call path.** Confirmed exhaustively (the same survey
  `decisions/ADR-030-session-scoped-codeact-wiring.md` independently ran for a different reason,
  re-confirmed here): no `Tool<>`-conforming spawn tool anywhere, no nested-agent-run invocation
  mechanism (the only existing "invoke another agent" function, `agent_registry.hpp`'s
  `invoke_agent_tool`, is tool-dispatch-only against the target's own FULL, unattenuated capability
  ceiling — explicitly not reusable for `agent.spawn`, per
  `docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md`'s own prior finding), no
  sub-worktree wiring connected to any spawn/handoff code, and `AgentSession` still owns no
  production sandbox/tool-call loop able to host a spawned child (ADR-024 §7). This ADR does not
  build any of that. `SpawnCostBudgetActor` is proven standalone, exactly matching ADR-006's own
  precedent for the depth half ("No test exercises this against a real `agent.spawn` call path,
  because none exists yet").
- **No refill/top-up mechanism** — a pool, once minted, only ever decreases. Named as deliberate
  (§2), not an oversight.
- **No attribution/audit trail** — `ConsumeSpawnTokens` carries no caller identity. A future real
  `agent.spawn` wiring would need to journal who spent what, separately.
- **Wall-clock/deadline budgeting is untouched**, unchanged from 026 §9 Q1's own "fully,
  explicitly open" framing.

## 6. Files changed

- `include/agentengine/trust/spawn_cost_budget.hpp` (new) — `ConsumeSpawnTokens`,
  `SpawnTokenGrant`, `SpawnCostBudgetActor`.
- `tests/test_spawn_cost_budget.cpp` (new) — this ADR's §4 evidence.
- `tests/CMakeLists.txt` — registers the new test target.
- `OpenQuestions.md` — OQ-14 amended: the cost half is now real, proven code (the standalone
  primitive), while `agent.spawn` itself remains unresolved and explicitly not claimed as solved.

Full regression suite: no new failures introduced by this ADR (verified against the pre-existing
baseline ADR-027/028/029/030 left — the one known failure,
`test_mediated_python_runner_hostile_corpus`, is the same pre-existing, unrelated failure, untouched
by any file this ADR changes). **Corrected 2026-08-11**: it was two real, deterministic
test-authoring bugs, not a flake; see ADR-024 §6's own corrected note.
