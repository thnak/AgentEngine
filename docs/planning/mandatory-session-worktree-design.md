# A clean design — every session mandatorily bound to a worktree, sandbox materialized from it

**Status: Revision 2. Not yet red-teamed.** Revision 1 (kept in git history, not reproduced here —
this file is meant to stay clean, not accumulate a revision log) routed the worktree/sandbox binding
through a `ContextProvider` (a composite class replacing `ADR-096`'s `SandboxToolProvider`). This
revision replaces that with a structurally different, simpler answer, per project-owner direction:
`ContextProvider`'s fan-out/chain split is itself downstream of a 2026-07-31 research pass grounded
in MAF, done before this codebase's worktree system existed as a real, materializable thing — a lot
of what `ContextProvider` does was invented to solve "what does the model see this turn," and the
worktree/sandbox lifecycle question doesn't actually belong to that problem at all. Stated plainly,
the reframing this revision is built on: **a session always has exactly one sandbox (which may be
"no sandbox"), and a sandbox always has a worktree inside it.** Worktree binding is session
*structure*, like `Principal` or `CapabilitySet` — not a contribution to the model's context.

## 1. The requirement, unchanged from Revision 1

1. Every `AgentSession` is bound to a worktree `Ref` from the moment it exists.
2. `fork_from()` — a background agent inheriting the FULL context of the main session — branches a
   new `Ref` (copy-on-write) from the parent's current tree.
3. `agent.spawn` — a single small request handled inside a running session — also branches, via the
   mechanism that already exists for it (`agent_spawn_worktree.hpp::mint_spawn_worktree()`).
4. Branching is undoable both ways: merge and reset to a checkpointed digest.
5. The session's shell sandbox is materialized FROM the bound Ref and harvested BACK into it.

## 2. The central reframe — `Sandbox` is `AgentSession` structure, not a `ContextProvider`

Every `AgentSession` gets one new, **mandatory, non-`ContextProvider`** member: a `Sandbox`. Two real
conformers:

- **`NullSandbox`** — the default for a session that never needs Shell/Python. Owns nothing but a
  worktree `Ref` (root, or branched from a parent — §1 items 1/2 apply to it exactly as much as to a
  real backend). No execution capability, no `SessionShellSandbox`, no `run_shell`.
- **A real backend conformer** (today: wraps `SessionShellSandbox`, `native_jail`) — everything
  `ADR-096`'s `SandboxToolProvider` did, now living inside `Sandbox` instead of inside a
  `ContextProvider`.

Both conformers ALWAYS own a worktree `Ref` — this is the literal meaning of "sandbox always has a
worktree inside it": rollback/checkpoint (§1 item 4) is a property of every session, independent of
whether that session ever runs a shell command. `Sandbox` is constructed once, at session
`initialize()` time (or `fork_from()`/spawn-branch time for a child) — not lazily inside a
`ContextProvider::on_context()` call the way `ADR-096` did it. This is the one substantive behavior
difference from Revision 1's design: the worktree bind is no longer deferred to first tool-call.

**Why this is simpler than making it a `ContextProvider` at all** (Revision 1's own approach, and the
reason the whole `fan-out`-vs-`chain` question kept resurfacing): a `ContextProvider` exists to
answer "what does the model see this turn." `Sandbox`'s lifecycle — construct once, branch on fork,
harvest+checkpoint at turn boundaries — has nothing to do with that question. Routing it through
`ContextProvider` anyway is exactly the kind of "invented because there was nowhere else to put it"
Revision 1 inherited from `ADR-096`, which itself inherited it from `ContextProvider` being the only
seam that existed when the worktree system was still a stub. Now that `Sandbox` is a first-class,
always-present `AgentSession` member, it doesn't need to fight for a place inside fan-out OR need a
new chain primitive — it was never a contribution-shaped problem to begin with.

## 3. What's left for `ContextProvider` to do — one thin, stateless reflector

The model still needs to see `run_shell` as a callable tool when the session's `Sandbox` supports it.
That's the ONE genuine contribution-shaped need left, and it gets the smallest possible fix: a thin
`ContextProvider` conformer (`SandboxToolReflector`, or folded directly into whatever composite a
host already uses) that holds nothing but a reference to the session's own `Sandbox` and, on
`on_context()`, contributes `run_shell`'s `ToolDescriptor` (if the bound `Sandbox` conformer supports
it) plus assigns `ctx.sandbox_fs`. It owns no construction state, no lazy materialization, nothing
that needs `engage()`-style rebuilding — reading a reference is trivially cheap to redo after a fork
(re-point it at `this` session's own, already-branched `Sandbox`, not the source's). This closes
Revision 1's biggest self-inflicted problem: there is no `engage()`-destroys-sibling-providers'-state
hazard anymore, because the thing that used to need fresh construction on every fork (`Sandbox`
itself) isn't inside the `ComposedContextProvider<Ms...>` tuple at all — only a stateless pointer to
it is, and repointing a stateless pointer costs nothing.

**How the reflector actually reaches `Sandbox`, concretely — corrected to a real, closer precedent.**
Neither `SessionContext` nor `EffectContext` carries a `Sandbox` reference — `on_context()`'s two
parameters have no seam for it. The closest real precedent is not `ToolDeclaringHistoryProvider`'s
lazy `configure()` (that shape exists specifically because Python's `CodeActRunnerBinding` has a
process-wide singleton-claim timing constraint `Sandbox` doesn't share) — it's
`src/backends/native_process/native_providers.hpp`'s real, shipped `NativeShellProvider`
(ADR-071): every instance is **constructor-injected** with an ALREADY-MATERIALIZED host directory
(`NativeShellProvider shell({"cmd"}, mount_root, "workdir");`) — that file's own top comment states
the rule directly: *"every provider instance is constructed with an ALREADY-MATERIALIZED real host
directory... this file does not re-implement that materialization, it consumes its output."* Same
idiom, for the sandboxed (native_jail) case instead of the unsandboxed one:
`SandboxToolReflector` takes `Sandbox const*` (already-constructed) **in its own constructor** — no
separate `bind_sandbox()` setter, no mutable rebinding. Propagation, precisely:
- `on_context()`: if `sandbox_->supports_shell()`, push `run_shell`'s `ToolDescriptor` into
  `ContextContribution.tools` and assign `ctx.sandbox_fs` — identical mechanics to `ADR-096`'s
  shipped `SandboxToolProvider::on_context()`, just reading an already-constructed `Sandbox` instead
  of lazily building one.
- `on_turn_end(TurnView, EffectContext&)`: does NOT go through the contribution channel at all — it
  produces no `ContextContribution`, so "propagation" here just means calling
  `sandbox_->harvest_and_checkpoint()` directly through the same pointer. Pure side effect, same
  shape `MemoryProvider::on_turn_end` already has for writing memory.
- `fork_from()`: constructs a FRESH `SandboxToolReflector` (pointing at `this->sandbox_`, the
  newly-branched `Sandbox`) and `engage()`s it onto the target session's `ComposedContextProvider<
  Ms...>` — never repoints `source`'s instance. Because the reflector holds no live resource of its
  own (only a pointer into `AgentSession`-owned state), constructing a fresh one costs nothing —
  the entire "rebuild" for this provider on fork is one cheap constructor call.

`ADR-096`'s C2 property ("composing the sandbox-owning type makes `fork_from()` fail to compile") is
no longer load-bearing the same way: the reflector holds no live resource to alias, so a plain field
copy of it during `fork_from()` is not a session-isolation hazard the way copying a real
`SessionShellSandbox`-owning object was — it's simply WRONG (points at the wrong session's Sandbox)
until corrected, not unsafe. `fork_from()` still needs to explicitly repoint it (§5), but this is now
a correctness fix, not a safety-critical compile-time guard being relied on to prevent aliasing.

## 4. Where "the worktree store" lives

Unchanged from Revision 1's own answer, still needed regardless of where `Sandbox` lives: a
type-erased `WorktreeStoreHandle` (`std::function`-wrapped `commit_ref`/`read_ref`/`get_tree`/
`put_tree`/`get_blob`/`put_blob`, mirroring `borrow_capabilities()`'s own store-bridging idiom),
built once via `make_worktree_store_handle(ObjectStoreT&, AppendLogStoreT&)` against any real
conforming store. `AgentSession` gets exactly one new, ordinary (non-template) member:
`std::shared_ptr<WorktreeStoreHandle const> worktree_store_` — avoids widening `AgentSession`'s
template signature (`<ChatClientT, StateT, HistoryProviderT>`) and the ~104-file blast radius that
would come with a fourth template parameter. Defaults to `nullptr`; a session with no store bound
gets a `Sandbox` that cannot actually construct a `Ref` (fails closed, not silently degrades) — this
is where "mandatory" is enforced, once a host opts in by calling `set_worktree_store(...)`.

## 5. `fork_from()` and `agent.spawn` — branch the ONE canonical `Ref`, not something buried in a provider

`fork_from()` (`agent_session.hpp:1161`) gains one real step, gated on `worktree_store_` being
bound: branch `this->sandbox_`'s `Ref` from `source.sandbox_`'s `Ref` via `create_sub_worktree(...,
sharing_mode::branch)`, constructing `this->sandbox_` fresh (never copying `source.sandbox_`, whose
concrete backend — if not `NullSandbox` — may own a live `SessionShellSandbox` that must not be
aliased across sessions, the same underlying hazard `ADR-096` C2 was protecting against, now
enforced by ordinary construction discipline in `fork_from()`'s own body instead of a compile-time
guard on a `ContextProvider`). Mirroring `agent_spawn_worktree.hpp`'s I2-1 fix: this step also
computes — but does not silently inject — a capability grant intersected against what
`source`'s own `CapabilitySet` held, returned to the host to fold into its own `set_capabilities()`
call (matching `fork_from()`'s existing, unchanged "capabilities_ deliberately not copied" contract).

`agent.spawn`'s `AgentSpawnToolProvider` (`include/agentengine/rt/agent_spawn.hpp`) already takes a
`caller_worktree_ref` constructor parameter — real, shaped plumbing, only ever exercised in tests
today. Once a session has `sandbox_->ref()` to hand it, wiring a real production construction site
is the remaining concrete step — no new design needed here.

## 6. Materialize once, harvest at turn boundaries

Unchanged in substance from Revision 1, now owned by `Sandbox` directly instead of by a
`ContextProvider`:

- **Materialize once**, at `Sandbox` construction (session `initialize()`/fork/spawn-branch time,
  not lazily on first tool call — the one behavior change from Revision 1, §2) — the real backend
  conformer's host directory is primed from the bound `Ref` via `materialize_mount()` before
  `SessionShellSandbox::create()` runs. `SessionShellSandbox` is then used exactly as today for the
  rest of the session — real filesystem semantics, `ExecState` persists naturally.
- **Harvest at turn boundaries.** Two ways to reach this, not yet decided between: (a) the thin
  reflector's `on_turn_end(TurnView, EffectContext&)` — already real, already wired at all 7 of
  `AgentSession`'s turn-boundary call sites — calls `sandbox_->harvest_and_checkpoint()`; or (b)
  `AgentSession::run_rounds()` calls `sandbox_->harvest_and_checkpoint()` directly at the same
  points, bypassing `ContextProvider` entirely for this too. (a) reuses an already-wired hook and
  needs no `agent_session.hpp` change; (b) is more direct but touches a large, heavily-tested file.
  Leaning (a) for the same "no new `AgentSession` mutator" reason `ADR-096` C1 preferred reusing
  `ContextProvider`'s existing seams — but this is exactly the kind of choice the next red-team pass
  should weigh, not something to decide unilaterally here.

## 7. Rollback — a real mechanism, decided now, not deferred

Per explicit project-owner direction: leaving rollback undesigned is exactly the failure this whole
redesign exists to close (session and sandbox not actually connected). §6 gave commit a real,
unconditional trigger (every turn boundary); rollback gets one here, not "maybe later."

**The operation**: `Sandbox::reset_to_turn(turn_index) -> result<void>`, a synchronous method on the
same `Sandbox` object §2 already made mandatory session structure. Mechanics, in order — steps 1 and
4 are NOT new: `core/worktree_ref_store.hpp` already has a real, tested, Judged
`rewind_to_turn(store, name, turn)` doing exactly that pair (`turn_digest_at` then `commit_turn`
reassignment) — `reset_to_turn()` calls it directly rather than re-deriving it:

1. Call `rewind_to_turn(*worktree_store_, sandbox_->ref_name(), turn_index)` — resolves the target
   tree digest from the Ref's own commit log AND commits a new log entry equal to it in one call
   (content-addressed "rollback" is always "commit a new state equal to an old one," the same shape
   `git revert` uses — the Ref's history is never erased, only moved forward to match a prior point).
2. Clear the real host directory's current contents — `std::filesystem::remove_all`/
   `create_directories` on the mount root, the SAME idiom `ADR-096`'s original `ensure_sandbox()`
   already used for first-time setup, now reused for a reset. Host-level session-lifecycle code, the
   same trust tier as the original directory creation — not a guest/tool-facing filesystem call, so
   it does not go through `FileSystemAdapter`'s mediated write path.
3. Re-run `materialize_mount()` against the target digest (from step 1's `TurnCommit::ref.tree_digest`)
   to refill the now-empty directory.
4. **`ExecState` reset, named explicitly rather than left as a landmine**: cwd/env may now reference
   paths that no longer exist post-rollback (cwd was `sub/`, which the rollback just removed).
   `reset_to_turn()` resets `ExecState` to its default (cwd = mount root, default env) as part of the
   same operation — a real, deliberate consequence of rollback, not a side effect a caller has to
   remember. Does NOT touch `SessionShellSandbox`'s own object identity — `fs_`/`registry_`/`shell_`
   stay alive at the same addresses (only the files under `fs_`'s fixed root, and the `ExecState`
   VALUE, change), so this is fully compatible with `SessionShellSandbox`'s "must never move after
   construction" constraint (§6) — nothing here reconstructs it.

**Trigger**: both host-driven (a direct `AgentSession`/`Sandbox` call — e.g. after a host-side policy
decision) and model-facing (a new tool, `reset_sandbox`, contributed by the same
`SandboxToolReflector` §3 already builds `run_shell` through) — reuses the SAME `cap::FsWrite` grant
`run_shell` already needs on the "work" mount, rather than inventing a new capability type: a caller
already trusted to write anywhere in the sandbox is already trusted to reset it, matching this
codebase's existing mount-level (not path-level) capability granularity for this kind of operation.

**Scope, stated precisely**: this resets ONE session's own `Sandbox`/`Ref`. A `fork_from()`-branched
child's rollback never touches the parent's Ref (COW branches are independent commit logs from the
moment they're created) — rolling back a child only ever affects that child's own history.

**Named residual, not solved here**: whether an UNMERGED child's rollback should also invalidate a
pending merge-back the host was about to perform — not analyzed; the merge/reset interaction (§1
item 4's other half, folding a child's changes back into the parent) still needs its own design pass.

## 8. Storage growth — no compaction exists, and the reason is structural, not an oversight

A real, direct question worth answering precisely rather than deferred: `rt::AppendLogStore`'s own
contract, stated verbatim in `worktree_ref_store.hpp`'s own comment, is *"never compacts... every seq
from 1..last_seq stays retained."* There is no GC/eviction method anywhere on
`InMemoryWorktreeObjectStore` either (`put_blob`/`get_blob`/`put_tree`/`get_tree` only — checked
directly, no `delete_blob`/`prune`/`gc`). A session harvesting+committing every turn, for its whole
lifetime, genuinely accumulates one ref-log entry per turn, forever, with no existing mechanism to
shrink it.

**This is not an oversight — it is the direct, unavoidable cost of the rollback guarantee §7 just
made mandatory.** `025-Worktree-and-Virtual-Filesystem.md` §9 G5 (the same gate `rewind_to_turn()`
implements) explicitly commits to "an arbitrary retained turn digest" being reachable — i.e. this
codebase's own spec already chose unlimited retention as a hard requirement, not an accident later
discovered to be expensive. Compacting away an old commit and being able to roll back to it are
directly opposed; you cannot have both without a retention LIMIT (keep only the last N, or last N
minutes/hours of checkpoints — anything older becomes unreachable). That is a real, different
feature, not "the same rollback mechanism, just more efficient."

**One real, structural mitigation already exists, worth stating so growth isn't overstated**:
storage is content-addressed. `put_blob`/`put_tree` key by digest — harvesting a file whose content
didn't change since last turn produces the SAME digest, and `try_emplace`/`insert_or_assign` do not
duplicate it. So BLOB/TREE storage growth is proportional to actual file CHURN (how much content
really changes turn to turn), not to turn COUNT or session duration by themselves — a long session
that leaves the sandbox mostly untouched costs almost nothing extra there. The ref-log entry itself
(one small `{digest, seqno}` record per turn) is the part that grows linearly and unconditionally
with turn count regardless of churn — small per entry, but genuinely unbounded over a long enough
session.

**Decided (project-owner direction, 2026-08-25): a real, host-configured policy is required, not
optional** — the write path is real host disk (the materialized sandbox directory today; the durable
`WorktreeObjectStore`, once Phase 4a ships, tomorrow), and unbounded growth against a physical
resource cannot be left to "accept it." Two host-declared knobs, mirroring `cap::FsWrite::quota_
bytes`'s own already-real, per-mount quota precedent (`trust/capability.hpp`) rather than inventing a
new shape:

- **`max_retained_turns`** (per session/`Ref`) — a rollback horizon. Checkpoints older than this many
  turns back become eligible for eviction. Optional; unset means "no per-session horizon" (still
  bounded by the store-wide quota below).
- **`max_store_bytes`** (per `WorktreeStoreHandle`, i.e. per underlying store instance, shared across
  every session bound to it) — the actual disk-protecting ceiling; bounds AGGREGATE usage across
  every session sharing one store, not just one session's own history, since most real deployments
  share one store process-wide (§4).

**Policy when a write would exceed either bound — host-configured, not silently decided by the
engine**, an explicit `retention_policy` choice, same "host declares, engine enforces" shape I8
already establishes everywhere else in this codebase (`ContextBudget`, `token_budget_`,
`SpawnBudget`):
- **`fail_closed`** (the default — matching this codebase's own preference for an explicit failure
  over silent data loss, `ContextBudget`'s own ADR-075 precedent: exceeding a declared ceiling fails
  the operation, never silently degrades): the commit that would exceed the bound is REJECTED —
  `harvest_and_checkpoint()`/`reset_to_turn()` returns an error naming which bound was hit. The
  session's real host-directory work up to that point is untouched (nothing is lost), but no further
  checkpoint can be recorded until the host intervenes (raises the quota, or explicitly evicts).
- **`evict_oldest`** (opt-in): before the write, GC the oldest retained checkpoint(s) beyond
  `max_retained_turns` (or beyond what fits `max_store_bytes`) to make room, then commit — a session
  never gets "stuck," at the real, disclosed cost of losing the ability to roll back that far. Every
  eviction is attributable (I4) — logged via the same kind of audit hook `SandboxBackendResolutionAuditHook`
  already establishes as this codebase's idiom for "an engine decision the host should be able to
  observe," not a silent trim.

**GC mechanics** (evicting a checkpoint = removing its ref-log entry and collecting any blob/tree no
longer reachable from ANY remaining retained checkpoint) are real, non-trivial follow-on work, not
designed further here — genuinely blocked on Phase 4a's durable `WorktreeObjectStore` actually
existing (today's `InMemoryWorktreeObjectStore` has no delete path at all, confirmed by direct read).
What IS decided now, not deferred: the POLICY CONTRACT (the two knobs, the two behaviors, fail-closed
as the safe default) — so Phase 4a's eventual store implementation has a real contract to build GC
against, rather than this question being reopened later from nothing.

## 9. Real limit — nothing structurally forces a write through the worktree-bound path

A genuine gap, named honestly rather than papered over. `Tool<>::invoke(Args, EffectContext&)` is
ordinary C++ — nothing prevents a native tool from writing to disk directly (raw `std::filesystem`/
`fopen` calls) instead of going through `ctx.sandbox_fs`. `EffectContext::sandbox_fs`'s own comment
already discloses this precisely: *"Deliberately NOT gated by any capability check of its own...
grants a native `Tool` nothing by itself... a tool that wants to use it must still perform its OWN
dynamic capability check."* It is the CORRECT path, not the ONLY reachable one — the trust boundary
for a native `Tool<>` conformer is the tool author/reviewer (host-vetted, compiled-in code, 009 §2),
not a compiler- or runtime-enforced wall. This is a pre-existing property of the whole `Tool<>`
model, not something this design introduces or worsens — but it also means this design does NOT, by
itself, make "every session's disk activity is captured by its worktree" universally true. Broken
down by real call path:

- **`run_shell`** — genuinely airtight: the shell process itself runs inside `native_jail`'s OS-level
  isolation, not merely in-process C++, so ALL of its I/O is structurally forced through
  `MediatedFileSystemAdapter` on the materialized directory. This is why harvest (§6) is complete
  for Shell specifically.
- **`execute_code`/Python — a real, currently-existing gap, not created by this design.**
  `tools/cli_chat.cpp`'s Python wiring uses its OWN separate scratch directory
  (`agentengine_cli_chat_workspace`), entirely disconnected from `Sandbox`/the worktree binding this
  design builds — named out of scope in §9 below, consistent with `ADR-096`'s own boundary, but
  worth being explicit about the consequence: **a session using both Shell and Python only gets
  commit/rollback coverage for the Shell half.** Python's writes are invisible to this design's
  worktree entirely, today.
- **Any other native `FsWrite`-capable tool** — relies entirely on the tool author actually reading
  `ctx.sandbox_fs` and performing the capability check themselves; nothing enforces it.
- **WASM tools** — checked `src/backends/wasm/wasm_tool_bridge.hpp` directly: no filesystem-write
  exposure to the guest found there at all (not fully verified beyond this one file) — if true,
  WASM guests may have a genuinely stronger, structural guarantee (an OS/memory-sandboxed guest
  cannot reach raw host syscalls regardless of what it wants to do), unlike native `Tool<>`
  conformers — worth confirming properly before relying on it, not asserted as settled here.

**Not solved by this design, and not claimed to be**: closing this gap for real would need either
(a) full worktree/Python integration (a real, separately-scoped redesign of Python's own
session-lifecycle wiring, explicitly out of scope per §10 below), or (b) some independent
detection/audit mechanism for a native tool writing outside its declared mount — neither attempted
here.

## 10. What this design explicitly does NOT do

- Does not reopen `OpenQuestions.md` OQ-18 — `Sandbox` isn't a `ContextProvider` at all, so the
  fan-out-vs-chain question OQ-18 settled doesn't even apply to it. The thin reflector (§3) is a
  completely ordinary fan-out member, no different in kind from `SkillsProvider`.
- Does not modify `turn_middleware.hpp`/`ADR-067`, `middleware.hpp`/`ADR-033`, `ADR-066`'s provenance
  stamping, or `ADR-074`'s `ComposedContextProvider<Ms...>` mechanics.
- Does not build a durable `WorktreeObjectStore` (still deliberately deferred; `WorktreeStoreHandle`
  is store-agnostic specifically so this can close later without touching this design).
- Does not design Python's own session-lifecycle wiring — same scope boundary `ADR-096` drew.
- Does not implement anything yet.

## 11. Open questions

### Resolved (project-owner decisions, 2026-08-25)

- **`Sandbox` selection reuses the existing `SandboxBackendRegistry` (ADR-080/098), not a new
  mechanism.** `build_default_sandbox_registry()` (`src/sandbox/default_sandbox_registry.hpp`,
  ADR-098, real and tested) already registers every compiled-in backend; `Sandbox` construction
  calls `registry.resolve_strict(current_platform())` — picks whichever backend family is actually
  available on THIS deployment (native_jail on Windows/Linux, wasm/kata if compiled in), the same
  ranked, deterministic resolution `check_sandbox_profile_availability()` already uses at agent
  registration time. If `resolve_strict()` returns nothing eligible, `Sandbox` is `NullSandbox` —
  fails closed to "no execution capability," never silently degrades. **This is the first real
  production consumer of a resolved `SandboxBackendRegistry` entry anywhere in this tree** — both
  ADR-080 and ADR-098 name "no real production consumption" as their own standing residual; this
  design closes it as a direct side effect, not as its own separate goal.
  - **Named precisely, not glossed over**: this resolves WHICH BACKEND FAMILY is available, not how
    Shell actually executes. `ADR-096`'s own Design A analysis already found `SandboxBackend::exec()`'s
    generic `ExecRequest const&, EffectContext& -> result<ExecOutcome>` signature too coarse for
    `SessionShellSandbox`'s persistent `ExecState`/`MediatedFileSystemAdapter` reference shape, and
    deliberately deferred routing Shell through it. This design keeps that: the registry answers
    "is native_jail available," and — when it is — `Sandbox`'s native_jail conformer still wraps the
    concrete `SessionShellSandbox`/`MediatedShellRunner` directly for actual execution, not the
    generic `SandboxHandle`/`exec()` path. Selection and execution are reused from two DIFFERENT
    existing mechanisms, deliberately, not one.
  - **Consequence for §5's `Sandbox`-interface-shape question**: since `RegisteredSandboxBackend`
    (`sandbox_backend_registry.hpp:72`) is already a plain runtime struct (`std::function`-wrapped
    `create`/`exec`/`destroy`, no template), `Sandbox` should be an ordinary RUNTIME member on
    `AgentSession`, not a template parameter — consistent with how the registry itself already
    works, and avoiding the same ~104-file blast radius §4 already ruled out for the store handle.
- **Harvested files are never automatically surfaced to the model.** The model reads what it wants,
  proactively, via an explicit read (the `tools/read_sandbox_file.hpp`/`read_content.hpp` shape) —
  not a "these files changed" message pushed after harvest. Matches 006 §7's own token-budget-hazard
  discipline (preview + `BlobRef`, never an automatic dump) exactly. This removes the entire
  "where does `harvest_mount()`'s `ContentItem` list go" question from Revision 1/2 — it goes
  nowhere model-facing; `harvest_mount()`'s return value is used only to confirm what was committed,
  not surfaced as content.

### Still open

- §9's gap (nothing structurally forces a native tool's disk write through the worktree-bound path;
  Python is a real, currently-uncaptured example) — not solved, needs its own scoped follow-up.
- §8's GC MECHANICS specifically (the policy contract itself — two knobs, two behaviors,
  fail-closed default — is decided; the actual eviction implementation is genuinely blocked on
  Phase 4a's durable `WorktreeObjectStore` existing).
- §6's (a) vs (b) choice for where turn-boundary harvest is triggered from.
- Exact shape of `WorktreeStoreHandle` and `fork_from()`'s new capability-template return value —
  both still sketches, not finished interfaces.
- Whether `clear_in_process_state()` should reset `sandbox_`/`worktree_store_`, and what that means
  for a pooled/reused session — not analyzed.

## 12. Round 2 red-team findings (2026-08-25) — the fork/engage story is broken again, not fixed

Three independent agents (security/I2-I3, C++/architecture correctness, fabrication-hunt) against the
full document (§1-§11). Recorded plainly, not silently fixed. Fabrication-hunt came back clean (8/8
claim clusters verified correct, one minor mis-citation — §7's "(§6)" for the `SessionShellSandbox`
move constraint should cite `session_shell_wiring.hpp`/`ADR-096` instead). The other two rounds found
real, several BLOCKING, issues — most seriously, the SAME class of bug round 1 already found once.

**BLOCKING — §3's fork_from()/`engage()` story is impossible the moment any other provider is
composed alongside the reflector; this reproduces, not fixes, round 1's central finding.**
`ComposedContextProvider<Ms...>::engage()` (`composed_context_provider.hpp:129-142`) takes the WHOLE
`std::tuple<Ms...>` at once and fails closed (`composed_context.already_engaged`) if the target is
already engaged — which any real session's composite already is, post-construction. So "construct a
fresh `SandboxToolReflector` and `engage()` it onto the target session" cannot mean what §3 says for
any session actually composed with `SkillsProvider`/`HistoryProvider`/etc. alongside the reflector —
`engage()` demands fresh instances of EVERY `Ms`, not just the reflector, the identical shape round
1's security finding already caught for a differently-shaped design. §3's own claim to have closed
that exact hazard by moving `Sandbox` out of the composed tuple does not survive contact with how
`engage()` actually works — the reflector's OWN re-binding is cheap, but getting it back INTO the
target's composite after a fork is not, and the document asserted it was without checking.

**BLOCKING — `WorktreeStoreHandle` (§4) type-erases the wrong operations; §6/§7's own call sites
won't compile against it.** `rewind_to_turn`/`turn_digest_at`/`commit_turn`
(`worktree_ref_store.hpp:137,150,174`) and `materialize_mount`/`harvest_mount`
(`worktree_mount_sync.hpp:139,161`) are all templated on the RAW `rt::AppendLogStore` concept
(`append`/`read_from`/`last_seq`, `rt/append_log_store.hpp:67-72`) — not on the higher-level
`commit_ref`/`read_ref` free functions §4 chose to type-erase. A handle exposing only
`commit_ref`/`read_ref`/`get_tree`/`put_tree`/`get_blob`/`put_blob` cannot satisfy what §7's
`rewind_to_turn(*worktree_store_, ...)` call (or §6's `materialize_mount`/`harvest_mount` calls)
actually need. The object-store half (`get_blob`/`put_blob`/`get_tree`/`put_tree`) is correctly
shaped; the ref-store half named the wrong six operations.

**Real-but-fixable — §7 step 4's `ExecState` reset has no real API to call yet.**
`SessionShellSandbox` (`session_shell_wiring.hpp:106-160`) exposes exactly `tool_descriptor()` and
`filesystem_adapter()` publicly — no accessor to its own `state_` (`ExecState`) at all. `reset_to_
turn()` as described needs a method that does not exist on the real class today; undisclosed scope,
not a design flaw in the idea itself.

**Real, security — `reset_sandbox`'s capability gate is weaker than `run_shell`'s own ceiling.**
§7 said this reuses "the SAME `cap::FsWrite` grant" — naming FsWrite only. But `reset_to_turn()`
resurrects an ENTIRE past tree, which is at least as much a disclosure (a caller gets host-directory
content it may never have held `FsRead` authority over) as a write — `read_sandbox_file.hpp`'s own
`find_fs_read` check is the pattern this skips. Should require BOTH `FsRead` and `FsWrite` on
"work", matching `run_shell`'s own declared ceiling (`RunShellTool`'s `Capabilities<FsRead<"work">,
FsWrite<"work">>`), not a unilateral downgrade to one axis.

**Real, security — `evict_oldest` (§8) can silently strand an unmerged `fork_from()` child, a
DIFFERENT and sharper mechanism than the "unmerged rollback" gap §7 already named.**
A branch's `base_digest` (`worktree_sub.hpp:35-39`) — required by `merge_trees()` for any real
three-way merge (`worktree_merge.hpp:158-160`) — is only reachable via the PARENT's own ref-log
entry at branch time, which is not itself something a per-Ref `max_retained_turns` GC scan on the
PARENT's own retention window would protect. A long-lived parent evicting its own old checkpoints to
satisfy its own quota can silently delete the exact tree a still-unmerged CHILD needs, breaking that
child's merge permanently with no warning at eviction time. Not named anywhere in §8 or §11.

**Real, security — §3's dismissal of ADR-096's C2 guard ("simply WRONG until corrected, not
unsafe") is unearned; it depends entirely on an ownership shape this design never commits to.**
Whether copying `Sandbox` during a stray `fork_from()` field-copy stays a compile error (safe) or
becomes a silent runtime aliasing hazard (the exact thing C2 existed to prevent) depends on whether
the concrete `Sandbox` type stays genuinely non-copyable (e.g. `unique_ptr<SandboxBase>`, mirroring
`SessionShellSandbox`'s own "must never move" discipline) — "ordinary runtime member, not a
template" (§11) does not by itself guarantee that. The design should mandate non-copyability
explicitly, not merely assert the downgrade is safe.

**Real, I4 — §9's disk-write-bypass gap is worse than described once §6/§7 exist: a bypassing
write is silently laundered into the audited commit, with no capability check and no attribution.**
`harvest_and_checkpoint()` must scan the real `mount_root` directory wholesale — it cannot
distinguish a mediated write from a native tool that bypassed `ctx.sandbox_fs` (§9 already concedes
this is possible). Those bypass writes land in the SAME directory and get swept into the next
commit with zero attribution, the opposite of §8's own "every eviction is attributable" standard
applied to ordinary commits. Also unanalyzed: `reset_to_turn()`'s `remove_all` (§7 step 2) racing a
bypassing tool's still-open file handle — a possible torn commit or sharing-violation failure mid-
rollback, not discussed anywhere in §7's four steps.

**Verdict**: this revision is markedly better on empirical accuracy than round 1 (fabrication-hunt
found almost nothing wrong), but the CENTRAL mechanism (§3's fork/engage story) failed for the exact
same structural reason round 1's design failed, restated with more confidence, not actually fixed.
Needs a real Revision 3 addressing: (a) what `fork_from()` actually does when the reflector is
composed alongside other providers — `engage()`'s all-or-nothing contract has not been designed
around yet, in either revision; (b) `WorktreeStoreHandle`'s real shape, matched against
`rt::AppendLogStore`'s actual concept; (c) `reset_sandbox`'s capability requirement; (d) cross-session
GC/merge interaction; (e) `Sandbox`'s non-copyability as an explicit, stated requirement.
