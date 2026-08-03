# ADR-006 — Are depth and budget bounds sufficient to contain `agent.spawn`'s recursion hazard, and how is the bound represented so guest code cannot widen it?

**Resolves (partially):** 026-Agent-Facing-Runtime-Surface.md §9 Q1, the sharpest case named by
OQ-14 (OpenQuestions.md). **Scope, deliberately narrow** (small prove, matching this backlog's
established scale): this ADR proves the depth-counter mechanism only. It does not build the rest of
`agent.spawn` (026 has no implementation yet — Draft status), does not address the wall-clock/token
cost per spawned run (a separate, already-tracked 023 budget question), and does not claim to close
every conceivable recursion-adjacent cost hazard — only the specific one 026 §9 Q1 names: unbounded
depth.

## 1. The question

007 §3's capability table already commits to `AgentCall<agent>` carrying "agent id, depth budget" as
parameters, and 007 §3 line 43 already asserts "delegation chains are recorded and depth-bounded" —
a stated but, until this ADR, unbuilt invariant. 026 §9 Q1 asks the honest follow-up: **are depth and
budget bounds actually *sufficient*, and can the representation be defeated by the code that holds
it** — since `agent.spawn` is model-written code creating new runs, exactly the shape I2/I3 exist to
keep out of a permission decision.

## 2. Background this design must respect

- **007 §3 rule 2 (attenuation only):** a spawned agent's budget must be a strict subset of its
  parent's — never equal, never wider.
- **007 §3 rule 4 (unforgeable in-process):** "handle types with private construction" — this is the
  *in-process* half of unforgeability 007 already names, distinct from ADR-005's cross-process
  cryptographic case. `agent.spawn` creates a sibling actor within the same engine/cluster (Quark),
  never crossing to a different OS process holding no shared type system — so the mechanism this ADR
  needs is the type-system one, not HMAC.
- **I2/I3:** model-generated code must never be able to construct or widen a budget directly; it can
  only trigger the *effect* (calling something that looks like `agent.spawn(...)` in generated
  Python), which the host mediates. This ADR's threat model is therefore narrower than ADR-005's: it
  assumes the mediation boundary (026 §5, 006 §3's tool pipeline) already holds — proving *that*
  boundary is a separate, already-scoped claim (006 §9 G4) — and asks only whether the counter itself
  can be defeated by any caller reachable *inside* the trusted host/engine code that does the
  mediating.

## 3. The design

`include/agentengine/trust/spawn_budget.hpp` — `SpawnBudget`, a single-field value type
(`remaining_depth_`) with:

- `mint_root(max_depth)` — the only way to create one with a caller-chosen depth; intended to be
  called once per top-level run from host policy, never reachable from guest code.
- `attenuate_for_spawn()` — the only other operation: consumes exactly one level and returns the
  child's budget, or fails closed (`spawn_budget.depth_exhausted`) if none remains. Check and narrow
  are one atomic call, not two, so there is no gap between "decided a spawn is allowed" and
  "actually reduced what the child receives."
- No public default constructor, no public constructor taking an arbitrary depth, no setter, no
  `widen`. Enforced by the compiler, not by convention — proven at compile time (§6).

No cryptography, no serialization, no `SecretKey` — deliberately simpler than ADR-005's
`CapabilityToken`, because the boundary this protects is a process's own type system, not a
cross-process byte string an untrusted party could fabricate from scratch.

## 4. Falsifiable claims

| # | Claim | Disproven by |
|---|---|---|
| S1 | `mint_root(N)` starts at exactly `N`. | Any other observed value. |
| S2 | `attenuate_for_spawn()` decrements by exactly 1 each call, for every starting depth. | Any call that decrements by a different amount, or that doesn't decrement. |
| S3 | The call that would take `remaining_depth()` below zero fails closed instead. | Any successful call at exactly the boundary, or any negative/wrapped depth observed. |
| S4 | Once exhausted, every subsequent call still fails — not just the first one past the boundary. | Any later call succeeding after an earlier one already failed. |
| S5 | `SpawnBudget` cannot be constructed from an arbitrary depth, or default-constructed, outside `mint_root`. | Code outside the class successfully constructing one by another path. |

## 5. The red-team attack

The adversarial question isn't "can an external party forge bytes" (there are no bytes to forge) —
it's "is there any path, at any starting depth, that produces a budget wider than attenuation should
allow, or that keeps succeeding past exhaustion." `tests/test_spawn_budget.cpp`:

- **S-R1** — after a budget is exhausted, ten further attempts are made against the *same*
  exhausted instance, confirming every single one fails, not just the exhaustion-triggering call
  (guards an off-by-one that only catches the exact boundary case and lets subsequent calls through).
- **S-R2** — exhaustive over every `max_depth` in `[0, 50]`: the chain is walked to exhaustion and it
  is confirmed the walk takes *exactly* `max_depth` successful steps, each decrementing by precisely
  1, before the next call fails — not spot-checked at one or two values.
- **Compile-time (not a red-team test but a positive control on the guard itself, §6):**
  `static_assert(!std::is_default_constructible_v<SpawnBudget>)` and
  `static_assert(!std::is_constructible_v<SpawnBudget, std::uint32_t>)` — these fail the *build*, not
  a test run, if the private-construction guard is ever weakened, which is a stronger guarantee than
  a runtime assertion for this specific claim.

**S-C4** (a zero-depth budget fails on its very first attenuation attempt) is the positive control
for the runtime checks: it proves the boundary fires exactly at zero, not "eventually," which is what
makes S-R1/S-R2's clean results meaningful rather than vacuous.

## 6. Executed evidence

MSVC 19.51.36252 (toolset 14.51.36231), Visual Studio 18 2026 generator, x64, Debug, `-j4`:
`test_spawn_budget` builds cleanly (header-only, links only `agentengine::core` — no crypto
dependency, unlike `capability_token`) and **all checks pass**, including the two `static_assert`s
above (which are load-bearing at compile time — the build itself would fail if either guard were
removed). Re-run under MSVC ASan (`build-asan-006/`, `-DCMAKE_CXX_FLAGS="/fsanitize=address"`, same
toolset): **identical pass, zero ASan findings.** UBSan not attempted (no clang toolchain on this
machine, same documented gap as ADR-005). Full existing CTest suite re-run afterward: no regression
(§8 of this session's work records the exact count).

## 7. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| S1 | **CORRECT** | S-C1 |
| S2 | **CORRECT** | S-C2, S-R2 (exhaustive over 51 starting depths) |
| S3 | **CORRECT** | S-C3, S-C4 (positive control) |
| S4 | **CORRECT** | S-R1 (ten repeated attempts, all fail) |
| S5 | **CORRECT** | Compile-time `static_assert`s in `tests/test_spawn_budget.cpp` |

## 8. The decision

**Accepted.** `SpawnBudget` is the representation for `AgentCall<agent>`'s depth-budget parameter
(007 §3). It answers 026 §9 Q1's depth half: **depth bounds, represented this way, are sufficient
against unbounded recursive `agent.spawn` — provided the mediation boundary that keeps guest code
from calling `mint_root`/`attenuate_for_spawn` directly holds** (an assumption inherited from 006 §9
G4, not re-proven here). 026 §9 Q1 is only *partially* resolved by this ADR: the cost half (wall-
clock, token spend, and resource accounting per spawned run) is a separate 023-budget question this
mechanism does not address and is not claimed to.

## 9. Residual risks and deferred gates

- **The mediation boundary is assumed, not proven here.** If guest Python code (once `agent.spawn`
  actually exists — 026 is still Draft) can reach `mint_root`/`attenuate_for_spawn` through any path
  other than the host's own effect-handling code, this entire proof is moot. That boundary is 006 §3/
  §9 G4's claim, already scoped separately; this ADR does not re-verify it and must not be read as
  having done so.
- **No budget dimension beyond depth is covered.** 026 §9 Q1 names "depth and budget bounds" as a
  pair; this ADR built only depth. Per-spawn cost (tokens, wall-clock, nested-run resource ceilings)
  needs its own mechanism and its own proof, tracked against 023, not silently assumed solved by this
  ADR's clean result on depth alone.
- **No test exercises this against a real `agent.spawn` call path**, because none exists yet (026 is
  Draft). This is a unit-level proof of the counter in isolation; integrating it into the real
  effect-handling path when `agent.spawn` is implemented needs its own verification that the
  integration actually calls `attenuate_for_spawn()` on every level, not just that the type itself is
  sound.
