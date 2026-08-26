# Mandatory session-worktree architecture — diagrams

Companion to `docs/planning/mandatory-session-worktree-design.md` (Revision 4, §1-§16). This file
only visualizes the CURRENT design — it carries no new decisions. Nodes/steps marked **(gap)** are
real, open findings from §14/§16's red-team rounds, not yet resolved; shown here so the diagrams
don't quietly imply more is settled than actually is.

## 1. Structure — what owns what

`Sandbox` is `AgentSession` structure (like `Principal`), not a `ContextProvider`. Only the thin
reflector sits inside the composed provider fan-out, and it owns nothing — just a pointer.

```mermaid
graph TD
    AS["AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;"]
    SB["Sandbox<br/>(mandatory, non-copyable — gap: not yet proven, §15.3)"]
    REF["ref_store_: ErasedAppendLogStore<br/>(optional, host-bound)"]
    OBJ["object_store_: ErasedWorktreeObjectStore<br/>(optional, host-bound)"]
    WREF["worktree Ref<br/>(branch/checkpoint history)"]
    SSS["SessionShellSandbox<br/>(native_jail backend only)"]
    FS["MediatedFileSystemAdapter"]
    ES["ExecState<br/>(cwd/env, persists across calls)"]
    CCP["ComposedContextProvider&lt;Ms...&gt;<br/>(fan-out, unchanged)"]
    REFL["SandboxToolReflector<br/>(thin, stateless, constructor-injected)"]
    TD["run_shell ToolDescriptor"]
    NULL["NullSandbox<br/>(no backend, Ref only)"]

    AS -->|owns, mandatory| SB
    AS -->|owns, optional| REF
    AS -->|owns, optional| OBJ
    AS -->|owns single slot| CCP
    CCP -->|one ordinary fan-out member| REFL
    REFL -.->|non-owning pointer| SB
    REFL -->|contributes, if supports_shell| TD

    SB -->|always owns| WREF
    SB -->|selects via SandboxBackendRegistry.resolve_strict| SSS
    SB -.->|no backend available| NULL
    NULL -->|still owns| WREF
    SSS --> FS
    SSS --> ES
    SSS -->|materialize/harvest| WREF

    classDef gap stroke:#c33,stroke-width:2px,stroke-dasharray: 4 2;
    class SB gap;
```

## 2. Session init — materialize once

```mermaid
sequenceDiagram
    participant Host
    participant AS as AgentSession
    participant SB as Sandbox
    participant Reg as SandboxBackendRegistry
    participant Store as ref_store_ / object_store_

    Host->>AS: initialize(session_id, principal)
    Host->>AS: set_worktree_store(ref_store, object_store)
    AS->>SB: construct (session init time, not lazy)
    SB->>Reg: resolve_strict(current_platform())
    alt backend available
        Reg-->>SB: RegisteredSandboxBackend (native_jail/wasm/kata)
    else none eligible
        Reg-->>SB: none — fails closed to NullSandbox
    end
    SB->>Store: read_ref(name) or create root Ref
    SB->>SB: materialize_mount(ref.tree_digest → host dir)
    SB->>SB: SessionShellSandbox::create(host dir)
    Host->>AS: history_provider().engage({Skills, History, SandboxToolReflector(&sandbox)})
    Note over Host,AS: engage() called explicitly by host — same as every other<br/>ComposedQuickstartSessionBuilder session, nothing new here.
```

## 3. Turn boundary — commit is unconditional

```mermaid
sequenceDiagram
    participant Model
    participant AS as AgentSession
    participant Refl as SandboxToolReflector
    participant SB as Sandbox
    participant Store as ref_store_ / object_store_

    Model->>AS: tool call (run_shell, ...)
    AS->>SB: dispatch via SessionShellSandbox (real OS-level jail)
    SB-->>AS: ExecOutcome
    AS->>AS: push tool-results message to history
    AS->>Refl: on_turn_end(TurnView, EffectContext&) — already wired, 7 real call sites
    Refl->>SB: harvest_and_checkpoint()
    SB->>Store: harvest_mount() → new Tree (content-addressed, dedups unchanged files)
    SB->>Store: commit_turn() → TurnCommit{ref, turn}
    Note over SB,Store: (gap, §9/§16) a bypassing tool's raw write into the same host<br/>directory is swept into this commit with no attribution.
```

## 4. `fork_from()` — branch, don't rebuild

The central fix across Revisions 2→3: `fork_from()` never tries to rebuild the composed provider.
It only prepares a fresh `Sandbox`; the host re-`engage()`s afterward, exactly like initial setup.

```mermaid
sequenceDiagram
    participant Host
    participant P as AgentSession (parent)
    participant C as AgentSession (child)
    participant SBc as Sandbox (child, fresh)
    participant Store as ref_store_ / object_store_

    Host->>C: fork_from(parent, "child-id")
    C->>SBc: construct fresh (never copies parent.sandbox_)
    SBc->>Store: create_sub_worktree(parent.ref, sharing_mode::branch)
    Store-->>SBc: SubWorktree{base_digest, backing_ref}
    Note over Store: base_digest protected from §8 GC while unmerged<br/>(gap, §16: no identity check on who may later abandon it)
    C->>C: history_provider_ left UNENGAGED (capabilities_ precedent extended, §3)
    Note over P,C: fork_from() computes a capability-intersection<br/>template (I2-1 style) but does NOT inject it —
    P-->>Host: suggested FsRead/FsWrite template
    Host->>C: history_provider().engage({Skills, History, SandboxToolReflector(&child.sandbox)})
    Host->>C: set_capabilities(...)
```

## 5. Rollback — `reset_to_turn`

```mermaid
sequenceDiagram
    participant Caller
    participant SB as Sandbox
    participant Store as ref_store_
    participant Shell as SessionShellSandbox

    Caller->>SB: reset_to_turn(turn_index)
    Note over SB: (gap, §15.1/§16) capability check ambiguous —<br/>cap::SandboxReset vs. Tool&lt;&gt;'s own static declaration unreconciled
    SB->>Store: rewind_to_turn(ref_name, turn_index)
    Store-->>SB: TurnCommit{new head = old digest} (history never erased, only moved forward)
    SB->>Shell: remove_all(host dir)
    Note over Shell: (gap, §9/§16) races a bypassing tool's still-open handle
    SB->>Shell: materialize_mount(target digest → host dir)
    SB->>Shell: reset_exec_state()
    Note over Shell: (gap, §7/§14) method doesn't exist on the real class yet
```

## 6. Storage-growth policy (§8)

```mermaid
graph LR
    Write["harvest_and_checkpoint()"] --> Check{"exceeds max_retained_turns<br/>or max_store_bytes?"}
    Check -->|no| Commit["commit — normal path"]
    Check -->|yes| Policy{"retention_policy"}
    Policy -->|fail_closed default| Reject["reject the write<br/>(session's real work up to now is untouched)"]
    Policy -->|evict_oldest, opt-in| Protect{"digest protected<br/>(live unmerged branch)?"}
    Protect -->|yes| Skip["never evict — even under pressure<br/>(gap, §16: SpawnCostBudget rate-limit<br/>and abandon_branch() both still broken)"]
    Protect -->|no| Evict["evict oldest, then commit"]
```

## Status legend

- Solid, no note → real primitive, already shipped and tested (`create_sub_worktree`,
  `rewind_to_turn`, `materialize_mount`/`harvest_mount`, `on_turn_end`'s 7 wired call sites,
  `SandboxBackendRegistry.resolve_strict`).
- **(gap)** annotations → open findings from the design's own red-team rounds (§14, §16) — not
  fixed, not hidden. See `mandatory-session-worktree-design.md` for the full text of each.
- Nothing in this diagram is implemented in `AgentEngine` source today — `Sandbox`, `SandboxToolReflector`,
  `ErasedAppendLogStore`/`ErasedWorktreeObjectStore`, `reset_to_turn`, `abandon_branch`, and
  `cap::SandboxReset` are all design-only as of this writing.
