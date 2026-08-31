# A clean design — every session mandatorily bound to a worktree, sandbox materialized from it

**Status: Revision 4 — closes §14's round-3 findings (new §15); §12/§13/§14 left intact as history.**
Revision 1 (kept in git history, not reproduced here —
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
- **`fork_from()` — CORRECTED in Revision 3 (§12's first BLOCKING finding, found twice now across
  two different design shapes).** Revision 2's claim ("constructs a fresh reflector and `engage()`s
  it onto the target's `ComposedContextProvider<Ms...>`") does not survive contact with `engage()`'s
  real contract: it takes the WHOLE `std::tuple<Ms...>` at once, not one member, and any session
  actually composed with OTHER providers (`SkillsProvider`, `HistoryProvider`, ...) alongside the
  reflector cannot have "just the reflector" rebuilt — `engage()` demands fresh instances of every
  `Ms`, and the ORIGINAL typed instances behind an already-engaged composite are unrecoverable
  (type-erased into `shared_ptr<Ms>`-capturing closures, `context_assembly.hpp`). There is no way to
  partially rebuild a `ComposedContextProvider<Ms...>` — this is not a bug to work around, it is the
  type's own one-shot-`engage()` design, already-Judged (ADR-074), not something this document gets
  to wish away.

  **The actual fix: stop trying to make `fork_from()` rebuild the composite at all.** `fork_from()`
  already has a real, working precedent for exactly this situation — `capabilities_` is
  "deliberately still NOT copied... needs `set_capabilities()` re-called after `fork_from()`"
  (`agent_session.hpp:1168-1171`, unchanged, pre-existing). Extend the SAME contract to
  `history_provider_`: `fork_from()` does **not** touch `child.history_provider_` at all — a freshly
  constructed `AgentSession` already starts with an unengaged `ComposedContextProvider<Ms...>` (its
  own default constructor, ADR-074), and `fork_from()` leaves it exactly that way. `fork_from()`'s
  only new responsibility (§5) is preparing `this->sandbox_` (the branched `Sandbox`, ready to be
  pointed at). **The host, not `fork_from()`, calls `child.history_provider().engage(std::tuple{
  SkillsProvider{...}, HistoryProvider<Window<N>>{}, SandboxToolReflector{&child.sandbox()}, ...})`
  afterward** — the identical shape a host already uses at INITIAL session construction (matching
  `ComposedQuickstartSessionBuilder`'s own `.engage()` call after `build()`), just called again,
  explicitly, post-fork. This is not a workaround — it is applying `fork_from()`'s own existing,
  already-correct pattern (for `capabilities_`) to a second field that has the identical shape of
  problem, instead of inventing a special-case auto-rebuild mechanism that `ComposedContextProvider`
  was never designed to support.

**Corrected in Revision 3 (§12's C2 finding).** Revision 2 claimed `ADR-096`'s C2 property ("composing
the sandbox-owning type makes `fork_from()` fail to compile") was "no longer load-bearing... simply
WRONG until corrected, not unsafe." Round 2's security review found this unearned: it is only true
if the concrete `Sandbox` type stays genuinely non-copyable — a property this document never actually
required. **Requirement, stated explicitly, not left implicit**: `Sandbox` MUST be non-copyable by
construction — e.g. `AgentSession` holds `std::unique_ptr<SandboxBase> sandbox_`, and `SandboxBase`'s
copy constructor/assignment are deleted, mirroring `SessionShellSandbox`'s own already-established
"heap-allocated, must never move" discipline (`session_shell_wiring.hpp:101-104`). Given this,
`sandbox_ = source.sandbox_` in a stray `fork_from()` edit is a compile error again — the SAME safety
property C2 always provided, now enforced by `Sandbox`'s own ownership shape instead of by
`ComposedContextProvider<Ms...>`'s copy-deletion (since, per §3's fix above, `Sandbox` is no longer
inside that composite at all). The guarantee moves, it does not disappear — but only if this
requirement is honored by whoever implements `Sandbox`, which is why it is stated as a MUST here
rather than assumed automatic.

## 4. Where "the worktree store" lives — CORRECTED in Revision 3 (§12's second BLOCKING finding)

Revision 2 type-erased the wrong six operations. `commit_ref`/`read_ref` are free functions built ON
TOP OF `rt::AppendLogStore`'s real concept — they are not what `rewind_to_turn`/`turn_digest_at`/
`commit_turn` (`worktree_ref_store.hpp:137,150,174`) or `materialize_mount`/`harvest_mount`
(`worktree_mount_sync.hpp:139,161`) actually require as their template parameter. All of them are
templated directly on the RAW concept:

```cpp
template <class T>
concept AppendLogStore = requires(T& store, T const& const_store, LogId const& id,
                                   std::vector<std::byte> bytes, SeqNo from) {
    { store.append(id, std::move(bytes)) } -> std::same_as<result<SeqNo>>;
    { const_store.read_from(id, from) } -> std::same_as<result<std::vector<std::vector<std::byte>>>>;
    { const_store.last_seq(id) } -> std::same_as<SeqNo>;
};
```
(`rt/append_log_store.hpp:67-72`, verbatim.) `WorktreeObjectStore`'s concept (`put_blob`/`get_blob`/
`put_tree`/`get_tree`, `worktree_types.hpp:99-105`) was already correctly identified in Revision 2 —
only the ref-store half was wrong.

**Fixed shape: two separate type-erased wrappers, one per real concept, each satisfying its concept
directly** (not a single handle exposing a hand-picked, wrong subset):

- `ErasedAppendLogStore` — a concrete class implementing `append`/`read_from`/`last_seq` by
  delegating to `std::function`-wrapped closures captured over a real `AppendLogStore`-conforming
  instance. Built via `make_erased_append_log_store(StoreT&)`, mirroring `borrow_capabilities()`'s
  "non-owning bridge" idiom. Because it implements the concept's exact three methods, it CAN be
  passed directly wherever `rewind_to_turn<StoreT>`/`materialize_mount<..., RS>`/`harvest_mount<...,
  RS>` expect a `StoreT`/`RS` satisfying `AppendLogStore` — this is the fix: the erased type itself
  models the concept, rather than exposing a different, incompatible surface.
- `ErasedWorktreeObjectStore` — the same idiom for `WorktreeObjectStore`'s four methods
  (`put_blob`/`get_blob`/`put_tree`/`get_tree`), built via `make_erased_worktree_object_store(StoreT&)`.
  Kept as Revision 2 designed it (correct as originally scoped).

`AgentSession` gets two new, ordinary (non-template) members —
`std::shared_ptr<ErasedAppendLogStore const> ref_store_` and
`std::shared_ptr<ErasedWorktreeObjectStore const> object_store_` — instead of one wrong-shaped
handle. Same blast-radius avoidance as Revision 2 (no fourth/fifth template parameter on
`AgentSession<ChatClientT, StateT, HistoryProviderT>`, no ~104-file churn). Both default to
`nullptr`; a session with either unset gets a `Sandbox` that cannot construct a `Ref` (fails closed).
"Mandatory" is enforced once a host opts in via `set_worktree_store(ref_store, object_store)`.

**Not fully resolved here, named as real follow-on scrutiny (unchanged from Revision 2's own §11 open
item)**: exact lifetime/thread-safety of the underlying concrete store the erased wrapper borrows
from, especially once multiple sessions (and, per §8, cross-session GC) share one store instance.

## 5. `fork_from()` and `agent.spawn` — branch the ONE canonical `Ref`, not something buried in a provider

`fork_from()` (`agent_session.hpp:1161`) gains one real step, gated on `ref_store_`/`object_store_`
(§4) being bound: branch `this->sandbox_`'s `Ref` from `source.sandbox_`'s `Ref` via `create_sub_worktree(...,
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

**New in Revision 3 — protects a branch's `base_digest` from §8's GC (closes part of §12's
cross-session hazard finding).** `create_sub_worktree`'s `branch` mode captures `base_digest`
(`worktree_sub.hpp:35-39`) — the common ancestor `merge_trees()` needs for a real three-way merge
(`worktree_merge.hpp:158-160`). This digest is reachable ONLY via the PARENT's own ref-log entry at
branch time; a per-Ref `max_retained_turns` GC scan on the PARENT's own retention window has no
reason to know a CHILD still depends on it. Fix: the same branch step that mints `base_digest` also
registers it as PROTECTED — `object_store_`'s eviction path (§8) must never collect a digest present
in a live protected set, checked before any `evict_oldest` pass runs. Protection is released when
the child either merges back (successfully folding its changes into the parent) or is explicitly
abandoned (a host-driven "discard this branch" call, not yet named as its own operation — a real gap
this note surfaces but does not fully design). **Not fully resolved**: this protected-set mechanism
itself needs its own scrutiny (where does it live — per-store, shared across every session using that
store, given eviction happens at the store level, not per-session; what happens if the HOST process
crashes with a child still unmerged and the protection registration was only ever in-memory).

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

1. Call `rewind_to_turn(*ref_store_, sandbox_->ref_name(), turn_index)` — resolves the target
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
4. **`ExecState` reset, named explicitly rather than left as a landmine.** cwd/env may now reference
   paths that no longer exist post-rollback (cwd was `sub/`, which the rollback just removed).
   `reset_to_turn()` resets `ExecState` to its default (cwd = mount root, default env) as part of the
   same operation — a real, deliberate consequence of rollback, not a side effect a caller has to
   remember. Does NOT touch `SessionShellSandbox`'s own object identity — `fs_`/`registry_`/`shell_`
   stay alive at the same addresses (only the files under `fs_`'s fixed root, and the `ExecState`
   VALUE, change), so this is fully compatible with `SessionShellSandbox`'s own "must never move
   after construction" constraint (`session_shell_wiring.hpp:101-104` — corrected citation, Revision
   3: Revision 2 mis-cited this as "(§6)"; that constraint lives in the real source file/`ADR-096`,
   not in this document's own §6) — nothing here reconstructs it.
   **Gap disclosed, not fixed in Revision 2, fixed here**: `SessionShellSandbox`'s real public
   surface today is exactly `tool_descriptor()` and `filesystem_adapter()` — no accessor to its own
   `state_` (`ExecState`) exists. This design REQUIRES a new public method,
   `SessionShellSandbox::reset_exec_state()`, resetting `state_` to its default — small, additive,
   same shape as every other opt-in accessor this class already has, but genuinely new surface, not
   something Revision 2 could silently assume already existed.

**Trigger and capability — CORRECTED in Revision 3 (§12's under-scoped-capability finding)**:
both host-driven (a direct `AgentSession`/`Sandbox` call — e.g. after a host-side policy decision)
and model-facing (a new tool, `reset_sandbox`, contributed by the same `SandboxToolReflector` §3
already builds `run_shell` through). Revision 2 gated this on `cap::FsWrite` alone; round 2's
security review found that insufficient — `reset_to_turn()` doesn't just write, it RESURRECTS an
entire past tree, disclosing host-directory content a caller may never have held `FsRead` authority
over (the same shape `read_sandbox_file.hpp`'s own `find_fs_read` check exists to gate). **Fixed
requirement**: `reset_sandbox` requires BOTH `cap::FsRead<"work">` AND `cap::FsWrite<"work">` — the
identical ceiling `RunShellTool` itself already declares
(`Capabilities<cap::decl::FsRead<"work">, cap::decl::FsWrite<"work">>`, `session_shell_wiring.hpp:77`)
— not a narrower, write-only gate for a strictly more powerful operation.

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
- **`max_store_bytes`** (per `ErasedWorktreeObjectStore` instance, §4 — corrected from Revision 2's
  now-split `WorktreeStoreHandle`, shared across every session bound to it) — the actual
  disk-protecting ceiling; bounds AGGREGATE usage across every session sharing one store, not just
  one session's own history, since most real deployments share one store process-wide (§4).
  **Interacts with §5's new protected-digest set**: `evict_oldest` must consult that set before
  collecting anything — a digest still referenced as a live, unmerged child's `base_digest` is
  never evicted regardless of age/quota pressure, even under `evict_oldest`. If honoring every
  protected digest would itself exceed `max_store_bytes`, that is a real, disclosed limit of
  `evict_oldest` as a policy — not resolved here, named for the next pass.

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

**Sharper than originally stated (§12's I4 finding, round 2) — a bypass write is worse than
"invisible," it is silently laundered as if it were an ordinary, attributable commit.**
`harvest_and_checkpoint()` (§6) must scan the real `mount_root` directory wholesale to build the
tree it commits — it has no way to tell a mediated write from a bypassing tool's raw write; both
physically land in the same directory. The bypass write is therefore swept into the NEXT
turn-boundary commit with no capability check and no attribution at all — the opposite of §8's own
"every eviction is attributable (I4)" standard, applied here to an ordinary commit rather than a GC
decision. Also unanalyzed until now: `reset_to_turn()`'s `remove_all` (§7 step 2) racing a bypassing
tool's still-open file handle on the same directory could produce a torn commit or an OS
sharing-violation failure mid-rollback. Neither is fixed here — both are real, sharper versions of
this same §9 gap, recorded rather than smoothed over.

## 10. What this design explicitly does NOT do

- Does not reopen `OpenQuestions.md` OQ-18 — `Sandbox` isn't a `ContextProvider` at all, so the
  fan-out-vs-chain question OQ-18 settled doesn't even apply to it. The thin reflector (§3) is a
  completely ordinary fan-out member, no different in kind from `SkillsProvider`.
- Does not modify `turn_middleware.hpp`/`ADR-067`, `middleware.hpp`/`ADR-033`, `ADR-066`'s provenance
  stamping, or `ADR-074`'s `ComposedContextProvider<Ms...>` mechanics.
- Does not build a durable `WorktreeObjectStore` (still deliberately deferred; §4's erased wrappers
  are store-agnostic specifically so this can close later without touching this design).
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
- Exact shape of `fork_from()`'s new capability-template return value — still a sketch.
- Whether `clear_in_process_state()` should reset `sandbox_`/`ref_store_`/`object_store_`, and what
  that means for a pooled/reused session — not analyzed.
- §5's new protected-digest set (Revision 3) — where it lives, its own thread-safety/crash-recovery
  story — named, not designed.
- The "explicitly abandon a branch" operation §5's protected-digest note assumes exists — it doesn't
  yet; releasing protection currently has only one real trigger (a successful merge), not two.

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

## 13. Revision 3 — per-finding disposition

| §12 finding | Severity | Disposition |
|---|---|---|
| `fork_from()`/`engage()` story impossible with other providers composed | BLOCKING | **Fixed (§3)**: `fork_from()` no longer touches `history_provider_` at all — extends the existing `capabilities_` "host re-supplies after fork" contract instead of inventing an auto-rebuild mechanism `ComposedContextProvider` doesn't support. |
| `WorktreeStoreHandle` type-erases the wrong six operations | BLOCKING | **Fixed (§4)**: split into `ErasedAppendLogStore` (matches the real `AppendLogStore` concept: `append`/`read_from`/`last_seq`) and `ErasedWorktreeObjectStore` (unchanged, was already correct). |
| `SessionShellSandbox` has no `ExecState` accessor | Real-but-fixable | **Named as a required new method (§7)**: `reset_exec_state()` — not yet implemented, but no longer silently assumed to exist. |
| `reset_sandbox`'s capability gate weaker than `run_shell`'s | Real, security | **Literal claim fixed, deeper issue still open (§14)**: does require both `FsRead`+`FsWrite` now (verbatim match, correctness-verified) — but round 3 found this treats a categorically more destructive operation as if an ordinary file-access grant already authorizes it, and `turn_index` is unbounded. |
| `evict_oldest` can strand an unmerged child's `base_digest` | Real, security | **Partially fixed, and a NEW gap found (§14)**: protected-digest mechanism named — but round 3 found it's a real, model-reachable DoS on the storage quota (unbounded `agent.spawn` branches, no release valve), not just "under-specified." |
| C2 dismissal ("simply WRONG, not unsafe") unearned | Real, security | **Requirement stated, NOT yet equivalent to C2 (§14)**: "MUST be non-copyable" is a design-doc sentence, not a compiled guard — round 3 found this is strictly weaker than C2's real, MSVC-proven compile error until an actual `Sandbox`/`SandboxBase` type ships with a real `static_assert`/deleted-copy-ctor. |
| Bypass writes silently laundered into audited commits (I4) | Real, not previously named this sharply | **Not fixed, sharpened (§9)**: still an open, disclosed gap — no mechanism proposed in this revision. |

**Not yet re-verified by an independent round**: everything in this section is this document's own
claim to have fixed round 2's findings — matching this project's own established caution (ADR-096
§8's own words apply here too: "the two rounds that found nothing wrong are evidence those specific
corrections happened to be right, not evidence the process has become more reliable"). A third
red-team round against Revision 3 specifically should not be skipped just because the fixes look
right on this document's own re-reading.

## 14. Round 3 red-team findings (2026-08-25) — verifying Revision 3's own fixes

Two independent agents (correctness-verification, security-verification), run specifically against
§13's disposition claims, not the whole document again. Recorded plainly.

**Correctness round — the two BLOCKING fixes are real at the design level, genuinely consistent
with `ComposedContextProvider`'s actual contract, but NOT yet implemented.** `agent_session.hpp:1178`
still contains the offending `history_provider_ = source.history_provider_;` line unconditionally —
the design's fix is a plan, not a landed change. Confirmed correct: a fresh `AgentSession`'s
`ComposedContextProvider<Ms...>` really does start unengaged (`composed_context_provider.hpp:76`)
and nothing touches `history_provider_` before `fork_from()` runs today, so removing that one line
and letting the host `engage()` afterward really would work as designed. `ErasedAppendLogStore`
confirmed buildable against the real concept with no signature obstacle. **§5's protected-digest set
is not just under-specified — it's incoherent with §4's own `ErasedWorktreeObjectStore` shape as
written**: a pure 4-method, non-owning forwarding bridge has nowhere to hold shared,
cross-session mutable protection state; a real design needs either a new method on that wrapper or
an entirely separate registry, neither named.

**Security round — three of the "fixed" rows in §13 do not actually close what they claim to,
found independently of the correctness round.**
- **§3's `Sandbox` non-copyability "MUST"** is a sentence in a document, not a compiled guard.
  `ADR-096`'s C2 was empirically proven by a real MSVC compile failure at an exact line; nothing
  today would catch a future implementer writing `Sandbox` as copyable (several other `AgentSession`
  members are still plain-copied in `fork_from()` today, `agent_session.hpp:1176-1178`) — the
  safety property has NOT actually moved yet, only been asserted as a requirement for when
  implementation starts.
- **§7's `reset_sandbox` capability fix closes the literal read/write-axis gap round 2 found, but
  not the deeper one.** Matching `run_shell`'s own `FsRead`+`FsWrite` ceiling assumes that ceiling
  is ITSELF sufficient authority for "discard N turns of real work in one call" — an unbounded
  `turn_index`, no check anywhere. This codebase already has precedent for giving a
  qualitatively-different operation its OWN capability kind (`cap::Schedule`, `cap::Background`) —
  "rollback authority" has no equivalent kind here; reusing an ordinary file-access grant for a
  wholesale-discard operation was asserted sufficient, never argued.
- **§5's protected-digest mechanism has a real, model-reachable DoS angle, not just missing
  detail.** `agent.spawn` is bounded by `cap::AgentCall`'s depth budget, not by any spawn-count/rate
  budget. A model that repeatedly branches (never merging) permanently protects one digest per
  spawn, forever — no release valve exists (the "abandon a branch" operation §11 already flagged as
  unbuilt is the ONLY named way to release protection, and it doesn't exist). This is a structural
  attack on §8's own storage-quota mechanism, reachable through ordinary, sanctioned tool use, not
  an edge case.

**Confirmed genuinely solid, both rounds agree**: the `fork_from()`/`history_provider_` fix's
DESIGN (not yet its implementation) and the `ErasedAppendLogStore`/`ErasedWorktreeObjectStore` split
are both real, correctly-cited, structurally sound. §9's bypass-write gap remains honestly
unfixed, not smuggled in as solved.

**Verdict**: Revision 3 is honest — no row in §13 overclaims something the real code contradicts —
but it is not yet "ready." Two of round 2's blocking findings are genuinely resolved AT THE DESIGN
LEVEL; the three security fixes are, on independent re-examination, closer to "identified and
partially addressed" than "fixed." A Revision 4 would need: a real bound on `reset_sandbox`'s
`turn_index` (or a dedicated capability kind for rollback authority); a real answer to the
protected-digest DoS (a spawn-rate/count budget, a mandatory abandon-on-timeout, or rejecting the
mechanism entirely in favor of accepting §8's original unbounded-growth-is-the-cost framing); and,
whenever implementation actually starts, `Sandbox`'s non-copyability proven the same way C2 was —
by a real compile probe, not by restating the requirement.

## 15. Revision 4 — closing §14's two fixable gaps; the third is not a design-doc problem

Two of §14's three findings have real fixes below, reusing already-real machinery rather than
inventing new mechanism (the discipline this whole document has tried to follow throughout). The
third (`Sandbox` non-copyability) is stated honestly as NOT closeable by a design revision at all.

### 15.1 `reset_sandbox`'s unbounded `turn_index` — a new, dedicated capability kind

Round 3's own comparison was the right one: this codebase already gives qualitatively different
operations their OWN capability kind rather than overloading an unrelated one — `cap::Schedule`
(`std::chrono::seconds max_horizon; std::uint32_t max_active;`) and `cap::Background` (`std::uint32_t
max_concurrent;`) are the real, existing precedent (`trust/capability.hpp:145-151`, with matching
compile-time `cap::decl::Schedule<MaxHorizonSeconds, MaxActive>`/`to_capability()` conversions,
`capability.hpp:390-391,455-458`). `reset_sandbox` gets the same treatment, not a reused `FsWrite`:

```cpp
// runtime (trust/capability.hpp, alongside Schedule/Background)
struct SandboxReset {
    std::uint32_t max_turns_back = 0;  // 0 == held but authorizes nothing; a real grant sets this
};

// compile-time declaration (cap::decl)
template <std::uint32_t MaxTurnsBack>
struct SandboxReset {};

template <std::uint32_t MaxTurnsBack>
[[nodiscard]] inline Capability to_capability(cap::decl::SandboxReset<MaxTurnsBack> const&) {
    return cap::SandboxReset{MaxTurnsBack};
}
```

`Sandbox::reset_to_turn(turn_index, EffectContext const& ctx)` now requires finding a held
`cap::SandboxReset` grant (a new `CapabilitySet::sandbox_reset_grant()` accessor, mirroring
`find_fs_read`/`find_schedule`'s own existing shape) AND checks `current_turn_index - turn_index <=
grant->max_turns_back` — fails closed (`sandbox.rollback_exceeds_grant`) otherwise. `run_shell`'s own
`FsRead`/`FsWrite` ceiling is UNCHANGED and no longer consulted for this operation at all — an
ordinary file-access grant no longer implies any rollback authority, closing round 3's actual
finding (not just the read/write-axis asymmetry Revision 3 closed). A host wanting "no rollback
beyond the last 5 turns" grants `cap::decl::SandboxReset<5>`; a host wanting no model-facing rollback
at all simply never grants it — `reset_sandbox` then isn't even contributed by `SandboxToolReflector`
(mirroring how `run_shell` itself is never contributed for a `NullSandbox`).

### 15.2 The protected-digest DoS — bound the rate via an already-real, already-unwired budget; add the missing release valve

Round 3 found the real gap correctly: `agent.spawn` has a depth budget (`trust::SpawnBudget`,
"strictly decreasing... every attenuation consumes exactly one level") but nothing bounding COUNT —
a session can branch without limit, each branch permanently protecting one digest with no way to
release it short of a full merge. Two closes, not one, because bounding the rate alone still leaves
"protected forever" once the (now-finite) budget is spent:

- **Bound the rate**: `agentengine::rt::SpawnCostBudget` (`rt/spawn_cost_budget.hpp`) already exists,
  is already real and tested, and is explicitly self-described as "proven standalone, NOT wired to
  any real `agent.spawn` call path — none exists yet in this codebase." This is the SAME shape of
  gap `ADR-098` closed for `SandboxBackendRegistry` (a real, tested mechanism with no production
  consumer) — reuse it rather than inventing a new counter: every `branch`-mode `fork_from()`/
  `agent.spawn` call consumes a fixed cost from the session's own `SpawnCostBudget` (host-initialized
  once, per `initialize(...)`, via `SpawnCostBudget::initialize(total_tokens)`) BEFORE
  `create_sub_worktree`/`mint_spawn_worktree` runs; exhausted budget fails the spawn/fork closed
  (`spawn_cost_budget.exhausted`, already a real error code). This bounds how many protected digests
  ANY ONE session can ever create, closing the "unbounded" half of round 3's finding with zero new
  mechanism — only a new call site for an already-Judged type.
- **Add the release valve** (§11's own previously-unbuilt "explicitly abandon a branch" operation,
  now designed): `Sandbox::abandon_branch(child_ref_name) -> result<void>` — releases that child's
  `base_digest` from the protected set WITHOUT requiring a merge, for the case where a branch's work
  is simply discarded, not folded back. Authorized by whoever could have spawned the branch in the
  first place (the same capability check `mint_spawn_worktree`'s own caller-side gate already
  performs) — not a new, separate capability kind, since abandoning a branch is strictly LESS
  powerful than having created it (discarding authority you already exercised, not gaining new
  authority). Once both `SpawnCostBudget` bounds creation rate AND `abandon_branch()` gives a real
  way to release a spent slot, a long-running session is never permanently stuck at its own budget
  ceiling the way an abandon-less design would leave it.

**Not fully resolved**: `SpawnCostBudget`'s own `initialize(total_tokens)` is host-configured, once,
per session — this design does not specify what a sensible default cost-per-branch or total budget
looks like (a product/tuning question, not an architectural one); and `abandon_branch()`'s
interaction with a merge ALREADY in flight (can a branch be abandoned mid-merge-attempt?) is not
analyzed.

### 15.3 `Sandbox` non-copyability — explicitly NOT closeable here; restated, not re-claimed

Round 3's finding stands as stated: a design document's "MUST be non-copyable" is not equivalent to
`ADR-096`'s C2, which was proven by an actual failed MSVC compile of real code. **This is not a gap
this document can close by writing more text** — the only real closure is implementation-time: when
`Sandbox`/`SandboxBase` actually ships, it must be proven non-copyable the same way C2 was (a
`static_assert(!std::is_copy_constructible_v<SandboxBase>)` at minimum, ideally the same kind of
throwaway compile-probe test ADR-096's own round 2 used). Recorded here as a REQUIRED verification
step for whoever implements this design, not something Revision 4 pretends to have resolved.

## 16. Round 4 red-team findings (2026-08-25) — §15's own fixes have new, real gaps

Two independent agents (correctness, security) against §15 specifically. Recorded plainly, not
silently fixed — matching this document's own now-established pattern (every prior "fix" round has
introduced or left at least one new real gap; this one is no exception).

**§15.1 `cap::SandboxReset` — STILL BROKEN, two independent problems.**
1. *(correctness)* The proposed `to_capability()` snippet doesn't compile as written: `Capability`
   is a closed `std::variant<cap::FsRead, cap::FsWrite, ...>` (`trust/capability.hpp:209`), and
   `capability_kind_of()`/`subsumes_payload()` are EXHAUSTIVE over every variant member
   (`capability_kind_of()` has a `static_assert(sizeof(T)==0, ...)` fallback that hard-fails to
   compile for any unhandled alternative, `:215-237`; `subsumes_payload()` needs its own overload,
   `:495-624`). Adding `cap::SandboxReset` requires touching the variant, the kind-switch, AND the
   subsumes overload set — §15.1 named none of this.
2. *(security)* Even granting that plumbing, §15.1 never states what happens to `reset_sandbox`'s
   own `Tool<>` declaration. Revision 3 declared `Capabilities<cap::decl::FsRead<"work">,
   cap::decl::FsWrite<"work">>` on it. `SandboxReset<MaxTurnsBack>` cannot cleanly replace that as a
   STATIC `Capabilities<...>` ceiling — `MaxTurnsBack` would be a single compile-time literal baked
   into one shared `Tool` type, not a host/session-configurable value. The real, closer precedent
   this design should have cited is `ScheduleWakeupTool` (`agent_session.hpp:385-394`), which
   declares **no** `Capabilities<>` at all for exactly this reason ("a compile-time ceiling could
   only check bare existence, never the live count") and enforces `cap::Schedule`'s horizon/count
   entirely via a dynamic `find_schedule()` check inside the tool body. If a real implementer instead
   LEAVES Revision 3's `FsRead`/`FsWrite` declaration in place and merely ADDS the new dynamic check,
   `tool_pipeline.hpp`'s capability-ceiling binding is AND, not OR (`:585-599`) — so a host would
   still need to grant ordinary file access just to unlock a tool this design claims is "no longer"
   gated by it, directly contradicting §15.1's own central claim.

**§15.2 `SpawnCostBudget` reuse — STILL BROKEN, three independent problems.**
1. *(correctness)* `SpawnCostBudget::consume()` is `task<result<SpawnTokenGrant>>` — a real
   coroutine (`rt/spawn_cost_budget.hpp:58`) — but `fork_from()` is a plain synchronous `void` method
   (`agent_session.hpp:1161`). §15.2 never states how a synchronous caller drives an async consume()
   call safely. The codebase's own real driving idiom (`agent_spawn_detail::drive()`,
   `rt/agent_spawn_child_run.hpp:94-97`) has an explicit safety precondition — a single worker thread
   guarantees only that thread can ever contend the budget's internal `AsyncMutex` — that `fork_from()`
   has no equivalent of. Naively driving `consume()` from an arbitrary, potentially-concurrent
   `fork_from()` call site risks resuming an already-parked coroutine handle out of turn, the exact
   hazard `SpawnPump` was built to rule out by construction. Not addressed.
2. *(correctness)* `SpawnPump`'s real constructor takes `SpawnCostBudget& cost_pool`
   (`agent_spawn.hpp:241`) — ONE shared instance across whatever flows through that pump, not
   "the session's own" as §15.2 describes. The design asks one mechanism to serve two different real
   ownership shapes without reconciling them.
3. *(security)* Even setting sync/async and ownership aside, a per-session budget is confirmed
   insufficient for what it claims to close: it has no cross-session or store-wide awareness at all
   (§8's real ceiling, `max_store_bytes`, is store-wide), and the design never states whether a
   session that itself calls `fork_from()` recursively gives each child a FRESH budget (per §15.2's
   own "host-initialized once, per `initialize(...)`" wording, this reads as yes) or a split share of
   the parent's remaining budget. If fresh, the "bounded" claim only holds for a single,
   non-recursively-forking session — materially weaker than §15.2 asserts.

**`abandon_branch()`'s capability claim — STILL BROKEN, a real cross-principal griefing vector.**
§15.2 said this reuses "the same capability check `mint_spawn_worktree`'s own caller-side gate
already performs." Verified false on two counts: (a) `mint_spawn_worktree`
(`agent_spawn_worktree.hpp:208-292`) performs no admission/denial check of its own at all — it only
reads `caller_held.fs_read_grants()`/`fs_write_grants()` to compute what grant to hand back; the
REAL gate is one layer up, in `perform_agent_spawn()`'s steps [2]/[3] (`check_and_consume_spawn_
depth`, `ctx.capabilities->attenuate(...)`, `rt/agent_spawn.hpp:376-396`) — a different function
than the one cited. (b) More seriously: `CapabilitySet` (`trust/capability.hpp`) has NO identity
field anywhere — every grant-lookup method (`find_fs_read`/`find_fs_write`/`contains`) checks
capability SHAPE only, never WHO is asking. So "authorized by whoever could have spawned the branch"
as designed actually authorizes ANYONE holding an equivalent `FsRead`/`FsWrite` grant on the SAME
`mount_id` — a sibling agent, or the branch's own child, can call `abandon_branch()` on a DIFFERENT
principal's still-useful branch and permanently destroy its ability to ever merge back. §15.2's own
"strictly less powerful than having created it" framing answers a capability-WIDENING question; it
never considers the identity/authorization question I2 also covers, and the real mechanism it cites
provides no identity check to lean on.

**Confirmed accurate**: §15.3's admission (non-copyability isn't closeable by a design document)
stands unchallenged by round 4 — the one part of §15 that wasn't found broken, because it never
claimed to be fixed in the first place.

**Verdict**: every mechanism §15 proposed to close round 3's findings has its own real,
independently-found gap — the `SandboxReset`/`Tool<>` interaction, the `SpawnCostBudget` sync/async
and ownership mismatch, and `abandon_branch()`'s missing identity check are not restatements of old
problems, they are NEW ones this revision's own fixes introduced. This is not implemented anywhere
in the tree today (no `SandboxReset`, `abandon_branch`, or updated `reset_sandbox` symbols exist) —
everything here is a forward-looking gap in the design text, not yet a shipped defect. A Revision 5
is not started here per instruction — recorded, then pushed as-is.
