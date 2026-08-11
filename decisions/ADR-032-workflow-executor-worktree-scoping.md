# ADR-032 — Workflow executor worktree scoping (014 §1 ↔ 025 §3)

**Status:** Proposed (2026-08-11). Designed, red-teamed (independent pass via the `Agent` tool,
findings in §3), implemented, and proven (real code + tests, §4); awaiting the project owner's
explicit "Judged" sign-off per this project's governance (`decisions/README.md`; `OpenQuestions.md`
OQ-11's resolution that the project owner is the ADR judge).

**Relates to:** `014-Workflow-and-Orchestration.md` §1 (the gap this ADR closes, quoted below);
`025-Worktree-and-Virtual-Filesystem.md` §3 (the sub-worktree/`sharing_mode` primitive this ADR
reuses without modification); `docs/architecture/worktree-sharing-skills-and-subagents.md` §3 (the
prior as-built trace: "designed, unit-tested at the primitive level, not wired" — this ADR wires the
policy+minting half, explicitly not the rest, see §5).

## 1. The question

014 §1 names an explicit, acknowledged gap, unresolved since it was first written:

> "Worktree scoping across executors is not yet specified here. 025 §3 owns the sub-worktree model
> (`/agents/<name>` sub-worktrees, `shared`/`branch`/`readonly`/`scratch` sharing modes) and that
> mechanism is real and tested at the `Ref`/`Tree` API level — but nothing in either RFC states
> whether a workflow executor or a handoff target gets its own sub-worktree, inherits the caller's,
> or shares one, and no code wires an executor to `create_sub_worktree` today."

**Stated so it has a wrong answer:** can a per-executor `sharing_mode` policy be decided and turned
into a real, capability-gated worktree grant using ONLY 025 §3's already-shipped, already-tested
`SubWorktree`/`Mount` primitives — no new object-store mechanism, no change to `WorkflowSupervisor`'s
execution semantics — or does closing this gap require touching either of those?

## 2. Scope, stated narrower than "wire workflows to worktrees" sounds

This ADR is the **policy + minting** layer only: deciding each executor's `sharing_mode`
(`Executor::worktree_mode`, `workflow/graph.hpp`) and turning it into a real `SubWorktree` plus a
guest-facing `Mount`/capability pair (`workflow/worktree_scoping.hpp`). It deliberately does **not**:

- Wire the resulting grant into `FunctionExecutor`'s `EffectContext`/`ExecutorBody` closures inside
  `WorkflowSupervisor`'s own construction path. No production host builds a `FunctionExecutor` fleet
  today (confirmed by grep over `src/`, `tools/` — tests only) — the same "prove the mechanism, wire
  it for real later" split `decisions/ADR-028-session-scoped-stateful-tools.md` already used for
  session-scoped stateful tools, for the identical reason: there is no real caller to design the
  wiring against yet, and inventing one would be designing for a hypothetical (CONVENTIONS.md).
- Touch `agent`- or `sub_workflow`-kind executors. `check_workflow_executable`
  (`workflow/graph.hpp`) still rejects both kinds outright — they are not built.
- Decide WHEN a `branch` executor's worktree merges back into its parent (025 §4's merge-on-join).
  This ADR mints branches; it adds no hook into `WorkflowSupervisor::execute()`'s superstep loop to
  ever fold one back.
- Attempt round-precise concurrency analysis as a smarter default than "always `branch`" (§3
  explains why, and why that would be unsound for this codebase's cyclic, dynamically-routed
  graphs).

## 3. Red-team findings and how each is addressed

An independent red-team pass (via the `Agent` tool, prompted adversarially against the first draft
design before any code was written) found one fatal flaw and six must-fix issues. All seven are
addressed in the shipped design; none were dropped as "acceptable residuals" without a fix, because
none of them were merely cosmetic.

| # | Finding | Severity | Fix |
|---|---|---|---|
| 1 | `readonly` sub-worktrees have no backing `Ref` (`SubWorktree::backing_ref_name` is empty by construction) but `Mount`/`mount_read`/`mount_write` have NO concept of a pinned-digest view — they unconditionally `read_ref(ref_store, mount.ref_name)`. Building a `Mount` for a readonly grant would 404 every read or, worse, alias every readonly grant in the system onto the same empty-string ref name. | **FATAL** | `ExecutorWorktreeGrant::mount`/`read`/`write` are `std::optional`, all `nullopt` together for `readonly`. A readonly executor still gets a real, correct `SubWorktree` (independently readable via `read_sub_worktree`) — just no `Mount`-level guest view yet. Fixing that for real needs a pinned-digest read path added to `Mount`/`mount_read` itself, a `core/worktree.hpp` change out of scope here (see §5). |
| 2 | Re-invoking the minting function on resume would silently re-branch from the parent's CURRENT digest via `create_sub_worktree`'s unconditional `commit_ref`, clobbering whatever the branch had already accumulated before suspension. | Must-fix | Split into `mint_executor_worktrees` (fresh run only) and `resume_executor_worktrees` (reconstructs `shared`/`branch`/`scratch` grants by re-deriving the same ref name and confirming it exists, never re-branching). `mint_executor_worktrees` also fails closed (`worktree_scoping.already_minted`) if called twice for the same parent ref, converting the hazard into a loud error rather than silent corruption. |
| 3 | The design's own suggested precondition — derive the per-run parent ref name from `WorkflowSupervisor::run_id()` — is temporally impossible: `run_id_` is assigned only inside `handle(Ask<RunWorkflow,...>)`, strictly after `initialize()`, which is where a `FunctionExecutor` fleet must already exist. | Must-fix | The precondition is restated honestly: the caller needs an independent pre-run identity source (not `run_id()`), which this ADR does not invent (real wiring is out of scope, §2). What IS enforced regardless of how the caller derives it: finding #2's existence-check-before-mint guard, so a caller that violates the precondition anyway gets a contract error, not silent aliasing. |
| 4 | `executor.id` was unsanitized before being spliced into a worktree ref name / guest-facing mount id (`"/agents/" + id`) — an id containing `/` or `..` is a path-shaped injection, contradicting 025 §5's "path escape is a security bug, not a bug." | Must-fix | Rejected in TWO places: `validate_workflow` (`workflow.executor_id_contains_slash`, so a graph carrying this never validates) AND `mint_executor_worktrees`/`resume_executor_worktrees` themselves (`worktree_scoping.executor_id_contains_slash`, defense in depth for a caller that skips `validate_workflow`). Proven in `test_workflow_worktree_scoping.cpp` M6. |
| 5 | The design's whole safety argument for defaulting every executor to `branch` depends on an author being able to opt a specific node into `shared` — but `TypedExecutor`/`WorkflowBuilder::add` (the actual C++ authoring surface) had no field or overload to ever set it. The escape hatch was unreachable. | Must-fix | `TypedExecutor<In, Out>` gained its own `worktree_mode` field (default `branch`), forwarded by `describe()`. Reachable via ordinary designated-initializer syntax before `WorkflowBuilder::add` — no new builder method needed. Proven in M11. |
| 6 | `branch` mode's initial content is the PARENT's entire current tree (025 §3's own copy-on-write semantics, not something this ADR introduces) — but the original proof plan only tested writes made AFTER branching, so it would have passed whether or not this was documented or even understood correctly. | Must-fix | Explicitly documented (this ADR, and worth restating: it is git-like whole-tree copy-on-write, not a scoped empty corner — `scratch` is the mode for that) and tested directly: M5 seeds the parent with pre-existing content BEFORE minting, then asserts the fresh branch grant can read it immediately, closing the "proof plan would pass regardless" gap the red-team called out by name. |
| 7 | Adding `#include "agentengine/core/worktree.hpp"` to `graph.hpp` (to reach the `sharing_mode` enum) would violate `graph.hpp`'s own stated contract — "THE GRAPH AS DATA, and nothing else" — dragging `quark::Store`/persistence/`trust/capability.hpp` into every lightweight consumer (a graph renderer, the YAML loader, the policy-reachability CI tool) that only ever wanted one enum value. | Must-fix | `sharing_mode` relocated to a new, dependency-free `core/sharing_mode.hpp` (nothing but the enum). `worktree.hpp` includes it and keeps using the unqualified name — no call site anywhere that already spells `agentengine::sharing_mode` needed to change (verified: `grep` found five files using it, all already `#include worktree.hpp` directly). `graph.hpp` now includes only the lightweight header. |

Two residuals surfaced during red-team were named, not built, and are restated in §5 rather than
silently dropped: `EffectContext::capabilities` ownership (a future wiring pass's problem, not
introduced here since this ADR never touches `EffectContext`), and the sibling-collision risk once
`sub_workflow`-kind executors exist (mitigated, not eliminated, by finding #4's fix — a real,
tracked future landmine).

## 4. The accepted design

- **`Executor::worktree_mode`** (`workflow/graph.hpp`): a `sharing_mode` field, default `branch`,
  **unconditionally** — not 025 §3's "sequential agents default to shared" rule replicated at
  per-node granularity. Reasoning: `WorkflowSupervisor::execute()`'s superstep model issues
  concurrent asks for every executor reachable in a round, not only explicit `fan_out`-kind edges —
  two ordinary `direct` edges out of one source behave identically (`route_from`, `supervisor.hpp`).
  General graphs here have cycles and dynamic `switch_case` routing (014 §9 Q2), so statically
  proving two specific nodes can never co-occur in a round would need a full round-reachability
  analysis that cycles and dynamic routing make unsound to do cheaply. Getting "these are safely
  sequential, default to shared" WRONG is a real cross-executor data-visibility hazard (025 §4's own
  warning about concurrent blind writes); defaulting to `branch` when `shared` would have been fine
  only costs an unnecessary, cheap (025 §3's own table) merge. An author who knows a specific node
  is safe to share sets it explicitly — mirroring 025 §3's own already-accepted precedent that
  `shared` for concurrent siblings "is never reached by a plain default."
- **`ExecutorWorktreeGrant`** (`workflow/worktree_scoping.hpp`): `{SubWorktree sub;
  std::optional<Mount> mount; std::optional<cap::FsRead> read; std::optional<cap::FsWrite> write;}`
  — the optionals are all-or-nothing, `nullopt` exactly for `readonly` (finding #1).
- **`mint_executor_worktrees(RS& ref_store, Ref const& run_parent_ref, Workflow const& wf)`**: for
  each executor (index-parallel output, matching `WorkflowSupervisor::initialize`'s existing `refs`
  convention), fails closed on a `/`-containing id (finding #4), fails closed if a worktree already
  exists for that executor under `run_parent_ref` (finding #2), then calls `create_sub_worktree`
  unmodified and builds the guest-facing `Mount`/capability pair for non-readonly modes.
- **`resume_executor_worktrees(RS& ref_store, Ref const& run_parent_ref, Workflow const& wf)`**:
  reconstructs `shared`/`branch`/`scratch` grants read-only (never mints), fails closed on
  `readonly` (`worktree_scoping.readonly_resume_unsupported`, finding #2's named residual — see §5)
  and on an executor with no prior mint (`worktree_scoping.resume_not_minted`).
- **`core/sharing_mode.hpp`** (new): the `sharing_mode` enum, relocated out of `worktree.hpp`
  (finding #7). `worktree.hpp` includes it; nothing that already used the qualified name changed.

## 5. What this ADR does not claim

- **No production `EffectContext`/`FunctionExecutor` wiring** (§2). The grants this ADR mints are
  real and independently usable (proven directly against `mount_read`/`mount_write`, §4's tests),
  but no code path threads one into a running executor's actual tool-call surface yet.
- **`readonly` executors have no guest-facing `Mount` at all** (finding #1) — fixing this needs a
  pinned-digest read path added to `core/worktree.hpp`'s `Mount`/`mount_read` themselves, a change
  to an existing, shipped, already-tested primitive header this ADR does not make as a drive-by.
- **`readonly` executors cannot be resumed** (finding #2's residual): `SubWorktree::pinned_digest` is
  captured only at mint time and lives nowhere durable (`readonly` never calls `commit_ref`).
  Reconstructing it needs it persisted as part of 014 §5's own checkpoint record (`RunStateRecord`,
  `workflow/checkpoint.hpp`) — a checkpoint-schema change this ADR does not make.
- **A resumed `branch` grant's merge ancestor (`SubWorktree::base_digest`) is not reconstructed
  either** — also not durably stored anywhere today. A resumed branch reads/writes correctly, but
  `merge_branch_into_parent` on one would need the ancestor re-supplied by the same future
  checkpoint-schema change named above.
- **Merge-on-join is not implemented** (§2) — this ADR mints branches, never folds one back.
- **`agent`/`sub_workflow`-kind executor worktree wiring is untouched** (§2) — those kinds don't
  exist yet.
- **`EffectContext::capabilities` ownership for a future wiring pass is not solved here** — noted by
  the red-team as a real footgun for whoever builds that pass (a borrowed, non-owning
  `CapabilitySet const*` with no natural owner yet in `WorkflowSupervisor`/`FunctionExecutor`),
  recorded here so it isn't rediscovered from scratch.
- **Sibling-id collision once `sub_workflow` executors exist** is reduced, not eliminated, by
  finding #4's `/`-rejection — a nested child's derived mount-id namespace and a top-level sibling's
  could still collide on a non-`/` substring match; a real, tracked landmine for that future work.

## 6. Falsifiable claims and verdicts

`tests/test_workflow_worktree_scoping.cpp`, eleven blocks, each tracing to a specific red-team
finding (cited in the test's own comments) rather than a generic happy-path sweep.

| # | Claim | Verdict |
|---|---|---|
| M1 | Grant shape is correct per mode: mount+read+write present for `shared`/`branch`/`scratch`, all three absent for `readonly`; a `readonly` grant still carries a correct `SubWorktree`. | **CORRECT** |
| M2 | Two `branch` siblings are isolated: a write through one's mount is invisible through the other's and never moves the shared parent — with a positive control proving the write IS visible through the writer's own mount. | **CORRECT** |
| M3 | Two `shared` executors observe each other's writes immediately (distinct mount ids, same backing ref). | **CORRECT** |
| M4 | A capability minted for executor A's mount is rejected against executor B's `Mount` — proving the grant wiring itself produces genuinely distinct mount ids, not merely that the underlying check works. | **CORRECT** |
| M5 | A fresh `branch` grant exposes the parent's PRE-EXISTING content immediately (finding #6, tested not asserted). | **CORRECT** |
| M6 | An executor id containing `/` is rejected by the minting function itself, independent of `validate_workflow` (finding #4). | **CORRECT** |
| M7 | A second `mint_executor_worktrees` call for the same parent ref fails closed and does not clobber the first mint's already-written content (finding #2/#11). | **CORRECT** |
| M8 | `resume_executor_worktrees`, called after a simulated restart (only the durable stores survive), reconstructs a `branch` grant that CONTINUES the same sub-worktree — a write made before the restart is still visible, and a write made after accumulates alongside it, not a fresh branch (finding #2's core fix). A resumed `shared` grant still backs onto the parent's own ref. | **CORRECT** |
| M9 | Resuming a workflow with a `readonly` executor fails closed with the documented residual code (§5). | **CORRECT** |
| M10 | Resuming an executor that was never minted fails closed rather than fabricating a fresh branch silently. | **CORRECT** |
| M11 | `TypedExecutor`'s `worktree_mode` field survives `describe()` into `Executor` — the escape hatch (finding #5) is reachable from the actual C++ authoring surface. | **CORRECT** |

Full regression suite: **184/185** pass. The one failure, `test_mediated_python_runner_hostile_corpus`,
is the same pre-existing, unrelated failure tracked separately (backlog item #38) — untouched by any
file this ADR changes. **Resolved (2026-08-11, backlog item #38):** not a flake — two real,
deterministic test-authoring bugs, both fixed the same day; see ADR-024 §6's own corrected note.

## 7. Files changed

- `include/agentengine/core/sharing_mode.hpp` (new) — the relocated `sharing_mode` enum.
- `include/agentengine/core/worktree.hpp` — includes the new header instead of defining the enum
  inline; no behavioral change.
- `include/agentengine/workflow/graph.hpp` — `Executor::worktree_mode`, `TypedExecutor::worktree_mode`
  (forwarded by `describe()`), and `validate_workflow`'s new `/`-in-id rejection.
- `include/agentengine/workflow/worktree_scoping.hpp` (new) — `ExecutorWorktreeGrant`,
  `mint_executor_worktrees`, `resume_executor_worktrees`.
- `tests/test_workflow_worktree_scoping.cpp` (new) — this ADR's §6 evidence.
- `tests/CMakeLists.txt` — registers the new test target (Windows-only, same real-`compute_digest`
  dependency as the other worktree tests it sits beside).
