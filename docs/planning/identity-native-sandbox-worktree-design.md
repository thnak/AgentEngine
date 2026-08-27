# A clean-slate stack — identity-native capabilities, a checkpoint ledger, and a sandbox that cannot be written around

**Status: Revision 1. Independent design track — not a revision of
`mandatory-session-worktree-design.md` (kept as-is, historical). That document tried to fit this
requirement onto the existing `Capability`/`AppendLogStore`/`WorktreeObjectStore`/
`SandboxBackendRegistry` primitives and found, across four revisions, that every fitting attempt
left a real gap (§16: missing identity on capability grants, a closed-variant extension tax, a
sync/async mismatch, an ownership-shape conflict). This document does not carry any of that
lineage forward and does not cite what it replaces — the design brief is to build the right thing
on its own merits, first-principles, at whatever size that takes. What survives untouched are the
project's own binding invariants (I1–I8, `AgentEngineSpecification.md` §4) — those are policy
properties of AgentEngine itself, asserted at the top of every RFC, and this document is bound by
them like any other change to this codebase. Reuse-vs-rebuild against existing headers is
explicitly an implementation-time question, not a design-time one — this document does not
optimize for it.**

## 0. The strategic bet

Most agent frameworks treat "the agent can run code" and "the agent's work is checkpointed" as two
separate, independently-optional features, often built by different teams at different times, glued
together loosely if at all. The bet this design makes: **fuse them into one non-optional structure**.
Every session — no exceptions, no opt-out — has exactly one execution surface (a sandbox, which may
decline all execution) and exactly one checkpoint lineage (a branch in a content-addressed ledger),
and the two are the same object as far as the session is concerned. A session cannot run code without
it being captured; a session cannot exist without being rewindable. That combination — mandatory,
zero-config, real rollback + real branch/merge, for every agent, every child agent, every tool call —
is the differentiator. It is also why this is being built fresh: retrofitting it onto machinery that
was never designed with identity or a ledger in mind kept surfacing the same shape of gap no matter
how the retrofit was reshaped (see the historical document's own round 4 verdict).

## 1. The requirement, restated as a spec, not a wishlist

1. Every session (top-level, `fork_from()` child, or `agent.spawn` child) is bound to exactly one
   `SandboxSession` from the instant it exists — never lazily, never optionally.
2. `SandboxSession` always owns exactly one **branch** in a **Ledger** (§4) — a content-addressed,
   append-only checkpoint history. A session with no execution capability still owns a branch; it
   just never writes to the working tree.
3. `fork_from()` (full-context inheriting background agent) creates a **child branch**, copy-on-write
   from the parent's current checkpoint, with a real, structural (not capability-shaped) record of
   who created it and why.
4. `agent.spawn` (single small request) creates a child branch the same way, through the same
   mechanism — no second branching code path.
5. A branch is always mergeable back or abandonable outright — both are first-class Ledger
   operations, not afterthoughts bolted onto a GC policy.
6. Rollback to any prior checkpoint on a session's own branch is a real operation, always available,
   bounded by a real, dynamically-checked authority (not a static ceiling that can't express "last N
   turns").
7. A `SandboxSession`'s working directory is synchronized with its branch's checkpoints; **no process
   given execution capability may hold a handle into the real working tree except through the one
   mediation surface this design defines** — closing the "bypassing tool write" gap structurally,
   not by disclaiming it as a known limitation.
8. Storage growth has a real, enforced, identity-scoped budget from day one — not a knob added after
   the fact once growth becomes a production problem.

## 2. The four primitives

Everything above is built from four new, independent primitives. None of them know about
`AgentSession`, `ContextProvider`, or `Tool<>` — they are a self-contained substrate a thin
integration layer (§9) wires into the rest of the engine.

```
┌─────────────────────────────────────────────────────────────────────┐
│  Principal & Grant  (§3)  — WHO is allowed to do WHAT, structurally  │
│                             carrying identity, not shape-only        │
├─────────────────────────────────────────────────────────────────────┤
│  Ledger              (§4) — content-addressed checkpoint/branch      │
│                             store; async-native; GC is a first-class │
│                             operation of branch lifecycle, not a     │
│                             side quest                                │
├─────────────────────────────────────────────────────────────────────┤
│  AsyncQuota<T>       (§5) — a generic, identity-scoped, coroutine-   │
│                             native budget: branch count, storage     │
│                             bytes, spawn depth all instantiate this  │
│                             ONE mechanism, not three different ones  │
├─────────────────────────────────────────────────────────────────────┤
│  SandboxSession      (§6) — binds one Ledger branch to one execution │
│                             surface; the ONLY writer of the working  │
│                             tree; harvest/materialize are atomic     │
│                             Ledger operations, never raw filesystem  │
│                             scans                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## 3. `Principal` and `Grant<T>` — identity is structural, not an afterthought

The single root cause behind the historical design's unfixable finding (§16's `abandon_branch()`
griefing vector) was that capability checks answered "is this SHAPE held" with no notion of "held BY
WHOM." This design makes identity a first-class part of the grant itself, not a side-table someone
has to remember to consult.

```cpp
// A Principal is an opaque, unforgeable identity value — minted exactly once per session/agent by
// the host at construction time (mirrors how AgentSession's own Principal member is minted today,
// but this type does not depend on that header — see the file banner on reuse-vs-rebuild).
class Principal {
public:
    [[nodiscard]] static Principal mint_root(std::string label);       // host-only entry point
    [[nodiscard]] Principal derive_child(std::string label) const;     // fork_from()/agent.spawn
                                                                         // use this — the child's
                                                                         // Principal carries a real
                                                                         // parent link, not just a
                                                                         // fresh random id
    [[nodiscard]] bool is_self_or_descendant_of(Principal const& other) const;
    [[nodiscard]] std::string const& label() const;
private:
    std::uint64_t id_;                    // process-random, unforgeable (no public raw constructor)
    std::optional<std::uint64_t> parent_id_;
};

// A grant is ALWAYS identity-scoped. `Payload` is any of the existing capability-payload shapes
// (FsRead, FsWrite, ...) OR one of this design's two new kinds (§7) — Grant<T> is generic over the
// payload, so identity is orthogonal to what kind of authority is being granted, not duplicated
// per-kind the way a naive "add an owner field to every struct" approach would.
template <class Payload>
struct Grant {
    Payload    payload;
    Principal  issued_to;              // the ONLY principal (or its structural descendants, see
                                        // below) who may exercise this grant
    Principal  issued_by;              // who minted it — audit trail (I4), not itself a check
    std::uint64_t grant_id;            // unique, unforgeable — a merge/abandon record cites this,
                                        // not a re-derived shape match, closing the exact ambiguity
                                        // that let a sibling's equivalent-shaped grant stand in for
                                        // the real one in the historical design
};

// The check every operation in this design goes through — shape subsumption (payload-specific,
// same kind of per-kind comparison the existing capability system already does well) AND identity.
// `caller` must be `issued_to` itself or a principal `issued_to` is a self-or-ancestor of — a CHILD
// exercising a grant issued to its parent is allowed (a sub-agent inherits what it was scoped to
// receive), the REVERSE is not (a parent's grant is never usable by a sibling or a stranger just
// because it's structurally reachable).
template <class Payload>
[[nodiscard]] bool authorized(Grant<Payload> const& grant, Principal const& caller,
                               Payload const& requested);
```

**Why this closes the griefing vector at the root, not as a patch**: `abandon_branch()` (§4) does not
perform a capability LOOKUP at all. A branch record's `created_by: Principal` field is the only thing
consulted — `abandon_branch()` requires `caller.is_self_or_descendant_of(branch.created_by)`,
full stop. There is no grant whose SHAPE could stand in for a different principal's identity, because
the check was never shape-based to begin with. This is a structural property of the Ledger's own data
model (§4), not a capability-system feature layered on top of it.

**Why a generic `Grant<T>` instead of teaching every payload struct about identity**: identity is
cross-cutting — every kind of authority (filesystem, network, rollback, branch-creation) needs the
same "who may exercise this" answer. Wrapping payloads generically means adding a new capability kind
never requires touching an identity story; it only requires the kind's own shape-subsumption rule
(unavoidable, and arguably a *feature*: an exhaustive, closed representation is what let this
project's engineers find a genuinely missing kind in the first place, rather than silently accepting a
guessed-at generic descriptor for it — see §7's honest acknowledgment of this extension cost).

## 4. `Ledger` — an async-native, branch-owning, GC-integrated checkpoint store

A `Ledger` is one coherent primitive replacing "a ref-log store plus a separate object store plus a
separate ad-hoc GC policy layered on after the fact" with one thing whose API makes an unmerged
branch's protection a property of *holding a `BranchHandle`*, not a side-registry someone might forget
to consult.

```cpp
// A checkpoint is a content-addressed tree, same conceptual shape as any Merkle checkpoint store —
// what's different here is the API surface around it, not the hashing scheme.
struct Digest { std::array<std::byte, 32> bytes; };   // BLAKE3 or SHA-256 — an implementation
                                                        // choice, not a design-level one
struct Checkpoint {
    Digest      tree;
    Digest      parent;           // {} for the branch's root checkpoint
    Principal   authored_by;      // I4: every checkpoint is attributable, structurally
    std::uint64_t turn_index;     // monotonic within a branch
};

// RAII-shaped, not a bare name string: holding a BranchHandle IS what protects a branch's
// checkpoints from GC — there is no separate "protected digest set" a design has to remember to
// wire up, because protection is encoded in the handle's own lifetime, not tracked out-of-band.
// Move-only (a branch has exactly one owning handle at a time; sharing a live handle across two
// call sites that might independently decide its fate is exactly the ambiguity the historical
// design's §5 left unresolved).
class BranchHandle {
public:
    BranchHandle(BranchHandle&&) noexcept;
    BranchHandle& operator=(BranchHandle&&) noexcept;
    BranchHandle(BranchHandle const&) = delete;             // C2-shaped, compiled-in from day one —
    BranchHandle& operator=(BranchHandle const&) = delete;  // see §9's non-negotiable requirement
    ~BranchHandle();   // if neither merge() nor abandon() was called, the destructor itself calls
                       // abandon() — a branch can never be silently forgotten and leak protection
                       // forever; RAII closes the historical design's DoS finding at the type level,
                       // not via a rate limiter alone (§5 still bounds CREATION rate; this bounds
                       // LEAK, a different failure mode neither §15.1 nor §15.2 of the historical
                       // document actually addressed)

    [[nodiscard]] std::string const& name() const;
    [[nodiscard]] Principal const&   created_by() const;
    [[nodiscard]] Digest             base_digest() const;   // the common ancestor merge() needs

private:
    friend class Ledger;
    BranchHandle(Ledger&, std::string name, Principal created_by, Digest base);
    Ledger*     owner_;
    std::string name_;
    Principal   created_by_;
    Digest      base_;
};

class Ledger {
public:
    // Every real operation is a coroutine — designed for this engine's coroutine substrate from the
    // first line, not retrofitted onto a synchronous free-function shape the way the historical
    // design's §15.2 tried and failed to reconcile with an already-sync fork_from(). fork_from()
    // itself becomes a coroutine in this design (§9) precisely so this is never a mismatch to paper
    // over.
    [[nodiscard]] task<result<BranchHandle>> create_root_branch(Principal owner);
    [[nodiscard]] task<result<BranchHandle>> branch_from(BranchHandle const& parent,
                                                          Principal created_by,
                                                          AsyncQuota<BranchCost>& quota);
        // `quota` is REQUIRED at the call site, not an internal implicit singleton and not
        // something a caller can forget to pass — see §5 for why the type system makes "unbounded
        // branch creation" a compile error, not a policy someone has to remember to wire in.

    [[nodiscard]] task<result<Checkpoint>> commit(BranchHandle const& branch, Digest new_tree,
                                                    Principal authored_by);
    [[nodiscard]] task<result<Checkpoint>> read_head(BranchHandle const& branch) const;
    [[nodiscard]] task<result<Checkpoint>> read_checkpoint(BranchHandle const& branch,
                                                             std::uint64_t turn_index) const;

    // Rollback commits a NEW checkpoint equal to an old one — history is never erased, matching the
    // append-only discipline the ledger's storage model already enforces structurally (there is no
    // delete/overwrite entry point on this type at all, the same "no compaction" property the
    // historical design's §8 confirmed was already correct and worth keeping as a design value, not
    // a limitation).
    [[nodiscard]] task<result<Checkpoint>> reset_to(BranchHandle const& branch,
                                                       std::uint64_t target_turn_index,
                                                       Principal requested_by);

    // A branch is resolved EXACTLY ONE of two ways — no third path, no leaking a live handle with
    // neither call ever made (the destructor's own abandon()-on-drop is the backstop, not the
    // primary contract):
    [[nodiscard]] task<result<Checkpoint>> merge(BranchHandle child, BranchHandle const& parent,
                                                   MergeStrategy strategy);
    [[nodiscard]] task<result<void>>       abandon(BranchHandle child);
        // both CONSUME the handle by value — a moved-from handle cannot be resolved twice, closing
        // the "double abandon / abandon-after-merge" race class at the type system, not by a runtime
        // check that has to remember to guard against it.

    // GC walks only what is UNREACHABLE from any live BranchHandle this Ledger currently holds open
    // — there is no separate "protected digest set" data structure to keep in sync with reality,
    // because reachability from a live handle *is* the protection rule. Runs opportunistically after
    // every merge/abandon (a branch's base_digest becomes GC-eligible the instant its handle is
    // resolved, not on a timer) and is itself a coroutine, so it never blocks a caller's commit path.
    task<void> collect_garbage();

private:
    // ... object/tree/blob storage, async I/O — an implementation detail deliberately not specified
    // at this layer; §11 names the honest open question about what backs it for a real deployment.
};
```

**Why RAII branch ownership instead of a name registered in a side table**: the historical design's
own §5/§16 gap was that "protection" was a fact recorded somewhere OTHER than the thing being
protected — a parent's GC pass had no way to know a child still needed a digest, because nothing
structural connected the two. Making the live `BranchHandle` itself the source of truth for
protection means there is no second data structure that can drift out of sync with which branches are
actually still open — the two other real findings this shape closes for free: a leaked reference
(never merged, never abandoned) cannot silently protect a digest forever (the destructor forces
resolution), and a branch cannot be abandoned twice or merged-then-abandoned (the handle is consumed).

## 5. `AsyncQuota<T>` — one generic, coroutine-native budget primitive

The historical design needed three different budget-shaped things (a spawn-cost budget, a
storage-bytes budget, a branch-count budget) and only ever built one of them (`SpawnCostBudget`),
which then turned out to have the wrong ownership shape and a sync/async mismatch with its one real
caller. This design builds ONE generic primitive and instantiates it three times, so there is exactly
one place a sync/async or ownership bug could hide, not three.

```cpp
template <class Kind>   // Kind is a tag type: BranchCost, StorageBytes, SpawnDepth, ...
class AsyncQuota {
public:
    // Quotas are IDENTITY-SCOPED from construction — a quota belongs to exactly one Principal, and
    // a child Principal's quota is a real, independently-observable SPLIT of a share the parent
    // explicitly allocated at derive_child() time, never a silently-fresh unlimited pool (closing
    // the historical design's round-4 finding that "does a recursively-forking child get a fresh
    // budget or a share of the parent's" was left unanswered).
    [[nodiscard]] static AsyncQuota mint_root(Principal owner, std::uint64_t total);
    [[nodiscard]] result<AsyncQuota> allocate_child_share(Principal child, std::uint64_t amount);
        // fails closed if `amount` exceeds what THIS quota has remaining — a child's share is
        // deducted from the parent's own pool at allocation time, so the sum of every descendant's
        // ceiling can never exceed the root total, structurally, not by convention.

    // The one real operation. Async by construction — every caller of this primitive is written
    // against a coroutine from day one (§9's fork_from() included), so there is no retrofit seam
    // where a sync caller has to invent its own driving idiom.
    [[nodiscard]] task<result<void>> try_consume(std::uint64_t amount, Principal spender);
        // fails closed (quota.exhausted) if amount > remaining, OR if spender is not this quota's
        // owner or a descendant it was split to (identity-scoped consumption, same Principal
        // discipline as §3 — a sibling cannot drain a quota that was never allocated to it).

    // The real release valve corresponding to Ledger::abandon()/merge() consuming a BranchHandle:
    // releasing a share back to the parent when a child's work concludes, so a long-running session
    // that spawns-and-abandons many short-lived children is never permanently worse off than one
    // that spawned fewer, larger ones.
    [[nodiscard]] task<result<void>> release_child_share(Principal child);

    [[nodiscard]] std::uint64_t remaining() const noexcept;

private:
    Principal      owner_;
    std::uint64_t  remaining_;
    AsyncMutex     mutex_;    // check-and-decrement as one atomic step, same discipline the
                              // historical design's SpawnCostBudget already got right in isolation —
                              // kept here because it's simply correct, not because it's reused code
};
```

**Instantiations**:
- `AsyncQuota<BranchCost>` — bounds how many branches a Principal (and its whole descendant subtree,
  via the split-share mechanism) may ever create. This is `Ledger::branch_from()`'s required
  parameter — the type signature itself makes "branch creation with no rate bound" impossible to
  write, closing the historical design's round-4 finding that a rate bound was never actually plumbed
  into the one real call site that needed it.
- `AsyncQuota<StorageBytes>` — bounds total committed bytes per Principal's branch subtree; `Ledger::
  commit()` consults this before writing (§8's storage-growth requirement, identity-scoped from day
  one instead of a single global knob).
- `AsyncQuota<SpawnDepth>` — the depth-bound half of `agent.spawn`/`fork_from()`, expressed the same
  generic way rather than as a separate, differently-shaped type.

## 6. `SandboxSession` — the one writer of the working tree

```cpp
class SandboxSession {
public:
    // Constructed once, at session init — never lazily. Owns its BranchHandle for its entire
    // lifetime; the handle is resolved (merge or abandon) only at session teardown or explicit
    // fork_from()/agent.spawn completion, never left ambiguous mid-session.
    [[nodiscard]] static task<result<SandboxSession>> create(Ledger&, BranchHandle branch,
                                                                ExecutionSurface surface);
        // `ExecutionSurface` is `none` (a session that never runs code — still owns a branch, still
        // rollback-capable, §1 item 2) or a real backend selection — which concrete execution
        // technology backs a real surface is explicitly out of scope for this document (§11).

    // Materializes the branch's current checkpoint into a private working directory that ONLY this
    // SandboxSession's own mediation layer can reach — see the write-mediation model below for what
    // "private" structurally means, not just "we didn't grant anyone else a handle."
    [[nodiscard]] task<result<void>> materialize();

    // Harvest is NOT a filesystem scan. Every write the execution surface performs is routed through
    // `MediatedWrite` (below), which stages each write as a content-addressed blob AND its logical
    // path AS THE WRITE HAPPENS — harvest_and_checkpoint() commits the already-known set of staged
    // writes into a new tree, attributed to whichever Principal's tool call produced each one. There
    // is no directory to "wholesale scan," so there is no bypass-write-vs-mediated-write
    // indistinguishability question to answer after the fact — the two are structurally different
    // code paths, and only one of them is capable of producing a commit at all.
    [[nodiscard]] task<result<Checkpoint>> harvest_and_checkpoint(Principal turn_owner);

    [[nodiscard]] task<result<Checkpoint>> reset_to_turn(std::uint64_t turn_index,
                                                           Grant<RollbackAuthority> const& grant,
                                                           Principal requested_by);
        // requires `authorized(grant, requested_by, RollbackAuthority{turn_index})` — a DYNAMIC
        // check (this kind is never given a static Capabilities<> ceiling — see §7) that the grant's
        // own max_turns_back actually covers the requested distance, and identity-scoped per §3.

    [[nodiscard]] MediatedFileSystem& filesystem();   // the ONLY handle any execution surface or
                                                        // native tool is ever given — see below
};

// The write-mediation model this design's §1 item 7 requires structurally, not by disclosed
// limitation:
//
// A SandboxSession's execution surface is NEVER hooked up to a real, writable bind-mount of the
// materialized directory. Every concrete backend (shell, an embedded interpreter, a native tool)
// receives a MediatedFileSystem reference instead — an interface, not a path — and every real I/O
// operation (open-for-write, rename, unlink, mkdir) is intercepted at this layer: the interpreter
// case (already-real prior art: guest Python's `builtins.open` is monkey-patched to relay through a
// host round trip rather than returning a real OS handle) generalizes here to EVERY execution
// surface, not just the one language runtime that happened to need it before. A native OS process
// (a real shell) that this design chooses to run gets its OWN private, per-invocation scratch
// directory (a tmpfs/overlay the OS actually enforces, never the branch's real materialized
// directory) — MediatedFileSystem stages each of ITS writes into the Ledger's blob store the same
// way an interpreter-mediated write would, then discards the private scratch directory at the end of
// the call. The materialized directory `materialize()` produces is therefore READ-ONLY input to an
// execution surface's own view of "what files already exist" — it is never itself the place new
// writes land. This is the one point at which this design spends real, non-optional engineering cost
// (an OS-enforced private scratch + explicit stage-and-discard per call, for every execution surface
// including a real native shell) to make the I4 attribution gap the historical design left open
// (§9/§16: "a bypassing write is silently laundered into the audited commit") structurally
// impossible rather than merely narrower.
class MediatedFileSystem {
public:
    [[nodiscard]] task<result<std::vector<std::byte>>> read(std::string const& path,
                                                               Grant<FsRead> const&, Principal caller);
    [[nodiscard]] task<result<void>> write(std::string const& path, std::vector<std::byte> data,
                                             Grant<FsWrite> const&, Principal caller);
    [[nodiscard]] task<result<std::vector<std::string>>> list_dir(std::string const& path,
                                                                     Grant<FsRead> const&, Principal caller);
};
```

## 7. Two new capability kinds, dynamically checked from the start

Following §3's generic `Grant<T>`, two new payloads are needed. Both are given **no static
`Capabilities<...>` ceiling** — a compile-time ceiling can only assert bare existence, never compare a
live runtime value (the exact shape this document's own precedent for that judgment already exists in
this codebase's `ScheduleWakeupTool`, cited here only as an engineering pattern worth following, not
as reused code) — so a tool contributing either of these performs the check inside its own body
against a `Grant<T>` the caller supplies via `EffectContext`, and contributes itself at all only when
`SandboxSession`'s execution surface is non-`none` (mirroring how a `run_shell`-shaped tool is never
contributed when there's nothing to run).

```cpp
struct RollbackAuthority { std::uint32_t max_turns_back = 0; };
struct BranchCost         { std::uint64_t cost = 1; };   // AsyncQuota<BranchCost>'s payload
```

`RollbackAuthority` is a genuinely new authority axis — a host that grants ordinary `FsRead`/`FsWrite`
grants no longer implies any rollback authority at all (closing the historical design's round-3
finding that reusing a file-access grant for a wholesale-discard operation was asserted sufficient,
never argued — here it is not even representable, since the two kinds are unrelated types).

**Honest cost of this choice, stated plainly rather than glossed over**: because `Grant<T>` is closed
over `T` at the type level (deliberately — see §3's own justification for exhaustiveness as a value,
not a limitation), a genuinely new authority kind is real, multi-site work every time: the payload
struct, its `authorized()`/shape-subsumption overload, and the tool-body wiring that checks it. This
document does not try to make that free — it tries to make it the ONLY kind of new-kind cost that
exists, by never also requiring a change to an identity story (§3 handles that once, generically) and
never requiring a decision about static-vs-dynamic ceiling placement per kind (§7's rule — dynamic,
always, for anything with a live/count-shaped parameter — settles that question for every future kind
the same way).

## 8. Session lifecycle, end to end

```mermaid
sequenceDiagram
    participant Host
    participant S as SandboxSession
    participant L as Ledger
    participant Q as AsyncQuota<StorageBytes>

    Host->>L: create_root_branch(principal)
    L-->>Host: BranchHandle (RAII, owns the branch until merge/abandon)
    Host->>S: SandboxSession::create(ledger, handle, surface)
    S->>S: materialize()  -- read-only view into a fresh working dir
    Note over S: execution surface gets MediatedFileSystem, never a raw writable mount

    loop each turn
        Host->>S: dispatch tool call (via MediatedFileSystem)
        S->>Q: try_consume(bytes_written, principal)   -- fails closed if over quota
        S->>L: harvest_and_checkpoint(turn_owner)       -- commits STAGED writes, not a dir scan
    end

    alt fork_from() -- full-context child
        Host->>L: branch_from(parent_handle, child_principal, branch_quota)
        L-->>Host: child BranchHandle
        Host->>S: SandboxSession::create(ledger, child_handle, surface)
    end

    alt rollback
        Host->>S: reset_to_turn(turn_index, rollback_grant, principal)
        S->>L: reset_to(branch, turn_index, principal)
        L-->>S: new Checkpoint (history untouched, head moved)
        S->>S: re-materialize working dir from new head
    end

    alt branch concludes
        Host->>L: merge(child_handle, parent_handle, strategy)  -- OR --
        Host->>L: abandon(child_handle)
        Note over L: either call consumes the handle; GC reclaims what's now unreachable
    end
```

## 9. Integration seam into the rest of the engine — deliberately thin, deliberately deferred

This document specifies four self-contained primitives and does not specify how `AgentSession`,
`ContextProvider`, or `Tool<>` wire into them — that is real, necessary follow-on work, but it is
integration work, not architecture work, and per the project's own process this size of change needs
its own design → red-team → prove → judge pass and ADR before it's decided, not a paragraph here. What
*is* stated now, as a non-negotiable carried over from this project's own prior, empirically-verified
finding (a real compile failure is the only thing that has ever actually proven this property for
`SessionShellSandbox`'s C2 requirement): **`BranchHandle` and `SandboxSession` are non-copyable, and
this must be proven by a compile probe (`static_assert(!std::is_copy_constructible_v<...>)` at
minimum) the moment either type is implemented — not asserted in a design document and left there.**

**Named explicitly, not left as an unstated "someday"**: a sandbox is not a leaf object — real work
happens *underneath* it. `Skill`s are mounted and read inside a session's sandbox mount today
(`MountSkillTool`, `ExternalSkillTool`); every `Tool<>` that touches the filesystem or spawns a shell
call is, structurally, an object operating *inside* whatever this design's `SandboxSession` owns; and
`ContextProvider`s (skills provider, history provider, and this design's own eventual reflector) run
in the same turn loop that `harvest_and_checkpoint()` drives. §9's deferral is about *how exactly*
each of these wires in — it is not license to treat them as afterthoughts once that wiring starts.
Two consequences flagged now, before integration design begins, precisely because round 1-2's pattern
was "a mechanism looks sound until something concrete tries to use it":
- **Skill mounts and `MediatedFileSystem` are the same kind of object** (a read/write view into a
  sandboxed mount) and must go through ONE mediation path, not two — a `Skill` mount that bypasses
  `MediatedFileSystem`'s staged-write discipline (§15.3) to read/write the sandbox mount directly would
  reopen the exact I4 bypass-write gap §6 exists to close, just via a different caller than a shell or
  native tool.
- **Every `ContextProvider` that runs inside the same turn as `harvest_and_checkpoint()` is a
  potential source of writes this design must account for**, not just shell/Python/native `Tool<>`
  calls — if a future `ContextProvider` (a memory/RAG writer, say) ever needs to persist something into
  the sandbox mount, it needs the same `MediatedFileSystem` sync facade every other write path uses,
  or it becomes a fourth, silently-unaudited write path alongside the three (shell, interpreter, native
  tool) this design already had to reason about explicitly.

Integration design should start FROM this list, not discover it fresh.

## 10. How I1–I8 are upheld

- **I1 (one session, one executor)**: `SandboxSession` owns exactly one `BranchHandle` and one
  execution surface for its whole lifetime; nothing about this design introduces a second mutator.
- **I2 (no ambient authority)**: every `Grant<T>` names who may exercise it (§3); `MediatedFileSystem`
  is the only path to a write, and it demands a grant on every call; there is no constructor anywhere
  in §3–§7 that produces a grant "for anyone."
- **I3 (model output is data, never authority)**: `Principal::mint_root`/`derive_child`,
  `Grant<T>` construction, and `AsyncQuota::mint_root`/`allocate_child_share` are all host-only entry
  points — nothing here takes a `TaintedText` or model-produced value as an authority input.
- **I4 (every effect is attributable)**: every `Checkpoint` carries `authored_by`; every `Grant<T>`
  carries `issued_to`/`issued_by`; §6's write-mediation model makes attribution structural for every
  write, closing the one real gap (bypass writes) the historical design left open.
- **I5 (nondeterminism crosses a recorded seam)**: `harvest_and_checkpoint()` is the one seam a
  session's real-world execution results cross into durable, replayable state — unchanged in spirit
  from the historical design's own §6, restated here on the new primitive.
- **I6 (declarative and native surfaces are equivalent)**: `RollbackAuthority`/`BranchCost` grants are
  ordinary values a declarative policy file can express exactly as a native `Capabilities<>` tag would
  — nothing in §7 is native-code-only.
- **I7 (protocol conformance is a gate)**: not directly engaged by this design (no wire protocol is
  introduced) — noted for completeness, not claimed as satisfied by omission.
- **I8 (budgets are enforced)**: §5's `AsyncQuota<T>` is the concrete mechanism; unlike the historical
  design's single, narrowly-scoped `SpawnCostBudget`, every quota this design needs (branch count,
  storage bytes, spawn depth) is the same primitive, checked the same way, with the same identity
  scoping.

## 11. Open questions — named honestly, not smoothed over

- **What actually backs `Ledger`'s object/blob storage for a real deployment** (in-memory for tests is
  obvious; a durable, crash-safe, content-addressed store is real, substantial engineering this
  document does not attempt to specify — sizing this is itself a multi-week piece of work).
- **What concrete execution technology backs a real `ExecutionSurface`** (an OS-level jail, a
  WASM runtime, a remote executor) and specifically how each one is made to honor §6's "no raw
  writable mount, ever" property — this is the single largest remaining engineering unknown in this
  document: for an OS-level native process this requires either kernel-level FS interception
  (FUSE/minifilter-shaped) or an accepted, disclosed narrowing (a private overlay per call, discarded
  after) — this draft assumes the latter but has not proven it holds against every syscall a hostile
  or merely careless process might issue (e.g. `mmap` a file then keep writing to the mapping after
  the "call" nominally ended — needs a real red-team pass specifically on this point).
- **Merge conflict resolution strategy** (`MergeStrategy` is named, not designed) — three-way merge of
  a tree is a real algorithm with real edge cases (renames, concurrent same-path writes) not
  specified here.
- **Cross-session/store-wide quota interaction** — §5's `AsyncQuota<T>` is scoped per Principal
  subtree; whether a store-wide ceiling ALSO needs to exist above every session's own tree (the
  historical design's §8 `max_store_bytes` was store-wide, not per-session) is not resolved — an
  argument exists for making the root `AsyncQuota<StorageBytes>` genuinely global rather than
  per-deployment-session, but this document does not commit to one.
- **`MediatedFileSystem`'s performance cost** — mediating every single read/write through a staged,
  content-addressed path is not free; whether this is acceptable for high-throughput tool use (e.g. a
  build step writing thousands of small files) is an open, unmeasured question, not assumed away.
- **What happens to a `BranchHandle` across a process crash** — the destructor's abandon-on-drop
  safety net (§4) only fires for a clean unwind; a hard crash mid-session leaves a branch with no live
  handle anywhere, which needs either a durable "reservation" record independent of any in-process
  object, or an accepted, disclosed leak-until-reap window. Not designed here.
- **`IdentityAuthority`'s own id space is not durable, and nothing here has specified how it should
  become durable if/when the answer to this section's own first bullet (durable `Ledger` storage) is
  built** — see §33, a real, CONFIRMED gap this exact combination produces, found by a dedicated
  post-prove-phase red-team round.

This is Revision 1. Per this project's own established practice for contested, security-critical
design (`CLAUDE.md`: "go through design → red-team → prove → judge"), the next real step is
independent, adversarial red-team rounds against this document specifically — not implementation.

## 12. Round 1 red-team findings (three independent agents against Revision 1)

Recorded plainly, not smoothed over — matching this project's own established discipline. This round
found the identity model (§3), the flagship RAII safety net (§4), the quota primitive (§5), and the
tool-integration seam (§6/§7) all broken at a more foundational level than any single round ever found
against the historical, machinery-reusing design across its four revisions. That is a real, notable
result on its own: building fresh did not avoid this class of problem, it relocated it.

**Fabrication-hunt (clean except one)**: every citation about existing invariants/prior art checked
out (I1-I8 vs `AgentEngineSpecification.md` §4, the Python `builtins.open`/`io.open` monkey-patch,
`ScheduleWakeupTool`'s real shape, ADR-096's C2 compile-probe history, no contradiction with CLAUDE.md's
locked decisions). One real error: §4's claim that the historical document's §8 "confirmed [no
compaction] was already correct and worth keeping as a design value" is backwards — that document's
§8 explicitly **mandates** a real, host-configured retention/eviction policy (`max_retained_turns`/
`max_store_bytes`) precisely because unbounded growth "cannot be left to 'accept it.'" This design's
`Ledger` had no such mechanism at all (only `collect_garbage()` for unreachable branches) — a real gap,
not just a citation error.

**BLOCKING — `Principal`/`Grant<T>` are forgeable; the identity model is the same class of hole it
was built to close, one layer down.** `Principal::mint_root`/`derive_child` are `public`, gated only
by a comment ("host-only entry point"), not a compiler-enforced choke point. `Grant<T>` is a bare
aggregate with public `payload`/`issued_to`/`issued_by`/`grant_id` fields and no minting gate anywhere.
Since `Principal` values are exposed as public fields/accessors all over the design
(`Checkpoint::authored_by`, `BranchHandle::created_by()`, `Grant<T>::issued_to`/`issued_by`) and are
freely copyable, any code that has ever *observed* one can call `.derive_child()` on it and hand-
construct a `Grant<T>` claiming to be issued to that fabricated "descendant" — `authorized()` has
nothing to check this against beyond the values inside the object it's handed. Concretely: observe a
root `Principal` anywhere (a log, a `Checkpoint`), derive a "child" from it, hand-build
`Grant<RollbackAuthority>{...}` naming that fabricated child, call `reset_to_turn()`. This is a direct
I2 violation, worse in one respect than the system it replaces: the existing, non-reused
`CapabilitySet` at least keeps its granted list `private` and never hands back raw fields a caller
could restructure into a self-issued grant.

**BLOCKING — multi-hop descent (grandchild via `fork_from()`-of-`fork_from()` or nested
`agent.spawn`) cannot work as specified even in good faith.** `Principal` stores exactly one hop
(`id_`, optional single `parent_id_`) — `is_self_or_descendant_of` cannot walk further than one level.
A legitimate grandchild trying to exercise a grant issued to the root has no stored fact connecting it
to the root at all. Fixing this needs either a caller-supplied full ancestor chain (reopens the
forgery finding above at every level) or a side-table the design's own §3 explicitly claims to have
eliminated ("not a side-table someone has to remember to consult").

**BLOCKING, confirmed independently by two separate reviewers — `~BranchHandle()`'s destructor-calls-
`abandon()` safety net cannot run.** `abandon()` is `task<result<void>>` — a real coroutine — and
`rt::task<T>` is lazily started (`initial_suspend()` is `suspend_always`) with a destructor that
destroys the frame outright if never resumed/awaited (`rt/task.hpp`, verified directly). A destructor
cannot `co_await` (illegal for a destructor to be a coroutine at all), so "the destructor calls
abandon()" can only mean constructing the task object and discarding it — which runs **zero lines** of
the abandon body. The flagship claim ("RAII closes the historical design's DoS finding at the type
level") is false as literally specified; the mechanism named to prevent the leak is itself a silent
no-op. One reviewer additionally traced a compounding hazard: since `abandon()` takes the handle by
value, the discarded task's own destruction re-destroys that by-value parameter, potentially
re-entering `~BranchHandle()` on an object that still looks unresolved.

**BLOCKING, same root cause — `collect_garbage()`'s "runs opportunistically... never blocks" has no
real driver.** Nothing in the design provides an executor/detached-task mechanism capable of
independently running a lazily-started, un-awaited coroutine. This is the identical assumption-of-a-
scheduler-that-doesn't-exist bug, occurring a second, independent time in the same document.

**BLOCKING — `MediatedFileSystem` is unreachable from the one place it has to be reachable from.**
Every `MediatedFileSystem` method is a coroutine; this project's real `Tool<Derived,
Policies...>::invoke` contract is synchronous `result<Reply>` (verified against `tool.hpp` and all 98
real conformers), deliberately so ("`ae::task<T>` stays deferred," per that header's own comment). A
tool body cannot `co_await` anything. §9's framing of engine integration as pure deferred follow-on
work undersells this: it is not a wiring detail, it is a shape incompatibility between this design's
own flagship write-mediation primitive and the only call site that would ever invoke it.

**BLOCKING — the `RollbackAuthority`/`Grant<T>`-via-`EffectContext` delivery story is both
internally contradictory and mischaracterizes its own cited precedent.** The real `EffectContext`
(read in full) has no field capable of carrying a `Grant<T>` — its only authority-shaped field is
`capabilities: shared_ptr<CapabilitySet const>`, the exact existing machinery this design declines to
reuse. And the real `ScheduleWakeupTool` this design cites as precedent does NOT check a per-call
grant inside its own tool body at all: its `invoke()` is explicitly unreachable, and the real dynamic
check happens in `StandingEffectRegistry::schedule_wakeup_impl()`, reached via a session-capturing
dispatch closure that bypasses `Tool::invoke` entirely and reads the session's *ambient* `CapabilitySet`
— a fundamentally different mechanism than the one described. Left as specified, the only realistic
path for an implementer to get any rollback-scoping value into a tool body at all is via
model-parsed `Args` — a direct I3 violation for the single most destructive operation in the whole
design.

**BLOCKING — `AsyncQuota<T>`'s `allocate_child_share` (declared synchronous) races `try_consume`
(coroutine, lock-guarded) on the same field, confirmed independently by two reviewers with a worked
double-spend.** `AsyncMutex::lock()` (this project's real type, read in full) has no synchronous
acquisition path at all — `allocate_child_share` structurally cannot take the same lock `try_consume`
uses. Worked scenario: `remaining_ = 1000`; a concurrent guarded `try_consume(200)` and an unguarded
`allocate_child_share(child, 900)` can interleave so the unguarded write (based on a stale read)
clobbers the guarded decrement, leaving the subtree having spent 1100 units against a 1000-unit root —
directly breaking I8, the invariant this primitive exists to uphold.

**BLOCKING — `release_child_share(Principal child)` has no amount parameter, no per-child allocation
ledger, and no anti-replay guard.** `AsyncQuota`'s only private state is `owner_`/`remaining_`/
`mutex_` — nothing records how much any given child was actually allocated, and nothing stops calling
`release_child_share` on the same child an unlimited number of times, each one crediting `remaining_`
upward with no floor. This is a one-line, unbounded budget-inflation exploit against the exact
mechanism §5/§10 credits with enforcing I8.

**BLOCKING — `Ledger::commit()`'s real signature carries no `AsyncQuota<StorageBytes>` parameter,
directly contradicting §5's claim that "`Ledger::commit()` consults this before writing."** Unlike
`branch_from()`, which makes its quota argument a required, structural parameter, storage-quota
enforcement as specified is either a secret internal singleton (the exact anti-pattern §5 exists to
avoid) or lives entirely in `SandboxSession`'s own orchestration as a convention every caller must
remember to follow — any other path reaching `commit()` directly bypasses it with nothing to stop it.

**BLOCKING (as literally diagrammed) / real-but-fixable in intent — §8's sequence diagram reuses a
`BranchHandle` after `SandboxSession::create()` has already moved it in.** `SandboxSession` is
described as owning its `BranchHandle` "for its entire lifetime," with no accessor returning it — the
diagram's later `merge(child_handle, ...)`/`abandon(child_handle)` calls reference a variable the host
no longer holds. Needs an explicit `release_branch()`-shaped accessor, which is a real API addition,
not a typo fix.

**Real-but-fixable — no `Grant<T>` enumeration/lookup container analogous to `CapabilitySet`'s eight
`find_*`/`*_grants()` accessors.** The existing (non-reused) `CapabilitySet` grew that surface because
real production bugs were found substituting a yes/no `contains()` check for a lookup that needs the
grant's own live parameters (documented in that file's own comments). This design silently drops that
lesson rather than replicating or addressing it.

**Real-but-fixable — `reset_to`'s "checkpoint equal to an old one" leaves `turn_index`/`parent`
ambiguous.** Whether the new checkpoint's `turn_index` continues the monotonic sequence or copies the
restored target's (colliding with it), and what `Checkpoint::parent` chains to (there is no self-digest
field on `Checkpoint` to chain against), are both unresolved — two implementers could reasonably build
incompatible, ambiguous checkpoint DAGs from the text as written.

**Real-security — native-shell write-mediation has concrete escape instances beyond what §11 already
(honestly) flagged, and §6's own prose overclaims relative to §11's hedge on the same mechanism.** A
double-forked/backgrounded child process can hold a write handle into a per-invocation scratch
directory past when mediation considers the call finished (POSIX delete-while-open semantics); a
symlink created inside the scratch dir pointing at a path the process can see may not be rejected
before an underlying OS `open()` resolves it, if the interception is path-based rather than fd-based.
§6 calls this mechanism something that makes the I4 gap "structurally impossible"; §11 in the same
document calls the identical mechanism unproven and merely assumed. Both cannot be true at once — §6's
language needs to drop to match §11's honest hedge, not the reverse.

**Confirmed genuinely solid, independently, by multiple reviewers**: `BranchHandle`'s
non-copyability (move-only, deleted copy ops) is real and compiler-enforced — no path to two live
handles over one branch was found. `merge()`/`abandon()` consuming the handle by value does prevent a
second, explicit resolution call on an already-resolved handle at the type level (the failure is
specifically in driving the *async* resolution to completion from a *sync* context, not in the
ownership/uniqueness model). The type-level separation of `RollbackAuthority`/`BranchCost` from
`FsRead`/`FsWrite` genuinely prevents an ordinary file grant from being mistaken for rollback authority
— independent of the (separately broken) delivery-channel problem. §11's own open-questions list was
judged honest and not defensively narrowed.

**Verdict (all three reviewers converge on this)**: not implementable as written; needs a real
revision addressing the identity/forgery foundation, the async/sync bridges (destructor, GC, quota),
the tool-integration delivery channel, and the commit/quota contradiction before another red-team round
is worth running against it.

## 13. Revision 2 — closing round 1's findings

Every BLOCKING finding above is addressed below by changing the actual mechanism, not by adding a
disclaimer. Three real lessons drove the shape of these fixes, stated once here rather than repeated
per-section: (1) **anything a tool body or model-adjacent code can observe must not be enough, by
itself, to mint or reconstruct authority** — every fix below either removes a public field that let a
value be copied into a forged object, or moves minting behind a single, host-exclusive choke point;
(2) **an operation that must "eventually happen but not block the caller" needs a real, named driver,
not a discarded coroutine** — every fire-and-forget claim below is replaced with either a genuinely
synchronous fallback path or an explicit queue plus a real, host-invoked drain step; (3) **`Tool<>::
invoke` is synchronous, permanently, by this project's own deliberate design choice — a primitive that
needs to be reachable from a tool body must present a synchronous facade at that one boundary**,
whatever it does internally.

### 13.1 `Principal`/`Grant<T>` — real unforgeability via a single, host-exclusive minting authority

```cpp
// The ONE object capable of minting a Principal or a Grant<T>. Never passed to a Tool body, never
// stored in EffectContext, never reachable from anything derived from model output (I3) — held only
// by the host's own session/orchestration code, the same posture CapabilitySet::grant_root() already
// establishes for the existing system (an entry point that exists and is greppable, but is reachable
// from exactly one place: host policy, never a tool).
class IdentityAuthority {
public:
    explicit IdentityAuthority();   // one per process/deployment root, host-constructed

    [[nodiscard]] Principal mint_root(std::string label);
    // The ONLY way to derive a child — requires proving control of the parent by requiring the
    // AUTHORITY ITSELF to perform the derivation, not the Principal value. A caller holding a mere
    // copy of a Principal (from a Checkpoint, a log, anywhere) cannot call this — it is a method on
    // IdentityAuthority, not on Principal, and IdentityAuthority is never exposed outside host code.
    [[nodiscard]] Principal derive_child(Principal const& parent, std::string label);

    // Every derivation is recorded in a REAL, internally-owned ancestry table — closing the
    // multi-hop gap directly: a grandchild's full ancestor chain is looked up here, not
    // reconstructed from a single stored parent_id field on the value type itself.
    [[nodiscard]] bool is_ancestor_of(std::uint64_t candidate_ancestor_id, std::uint64_t descendant_id) const;

    // The only way a Grant<T> is ever created. Requires the authority's own record that `issued_to`
    // is a real, previously-minted Principal (rejects a fabricated id outright) — closing the "hand-
    // build a Grant<T> naming an observed Principal" attack at the source, not by hoping nobody
    // tries.
    template <class Payload>
    [[nodiscard]] Grant<Payload> mint_grant(Payload payload, Principal issued_to, Principal issued_by);

private:
    struct AncestryRecord { std::uint64_t id; std::optional<std::uint64_t> parent_id; };
    std::unordered_map<std::uint64_t, AncestryRecord> ancestry_;   // host-process-lifetime, append-
                                                                     // only — a Principal, once
                                                                     // minted, is never un-minted
    std::atomic<std::uint64_t> next_id_{1};
    AsyncMutex mutex_;   // guards ancestry_/next_id_ under concurrent mint/derive calls — every real
                          // mutator on this type is now a coroutine (see 13.3's discipline), closing
                          // the exact sync/async split that caused §5's original race
};

// Principal itself is now a bare, copyable, IDENTITY-ONLY value — an opaque id, nothing more. It has
// NO public mint_root/derive_child of its own anymore (both moved onto IdentityAuthority above); it
// is safe to expose freely on Checkpoint/Grant<T>/BranchHandle precisely BECAUSE possessing a copy of
// one no longer grants any minting power — the previous design's core mistake was conflating "an
// identity value" with "the authority to mint related identity values," and this fix separates them.
class Principal {
public:
    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] std::string const& label() const;
    // is_self_or_descendant_of() is DELETED as a method on this type — that question can only be
    // answered by consulting IdentityAuthority::is_ancestor_of(), which is the one place the real
    // ancestry table lives. authorized() below takes the IdentityAuthority explicitly so this can
    // never be silently skipped.
private:
    friend class IdentityAuthority;
    Principal(std::uint64_t id, std::string label);
    std::uint64_t id_;
    std::string   label_;
};

// Grant<T> is no longer a bare public aggregate — payload is readable (a caller needs to see what
// it's checking), but issued_to/issued_by/grant_id are private, with the authorized() check as the
// ONLY way to ask a question about them. This closes the "harvest issued_to off a Grant<T> and
// reuse it to hand-build a forged Grant<T>" attack: there is no public constructor that takes an
// arbitrary issued_to and produces a Grant<T> outside IdentityAuthority::mint_grant() above.
template <class Payload>
class Grant {
public:
    [[nodiscard]] Payload const& payload() const;
private:
    friend class IdentityAuthority;
    Grant(Payload payload, Principal issued_to, Principal issued_by, std::uint64_t grant_id);
    Payload      payload_;
    Principal    issued_to_;
    Principal    issued_by_;
    std::uint64_t grant_id_;
};

template <class Payload>
[[nodiscard]] bool authorized(Grant<Payload> const& grant, Principal const& caller,
                               Payload const& requested, IdentityAuthority const& authority) {
    // shape check unchanged in spirit from Revision 1; identity check now walks the REAL ancestry
    // table (multi-hop, correctly) instead of comparing a single stored parent_id field.
    return authority.is_ancestor_of(grant.issued_to_id(), caller.id()) /* or equal */
           && subsumes_payload(grant.payload(), requested);
}
```

**What this closes**: Finding 1 (forgery) — there is no public path to a `Grant<T>` at all outside
`IdentityAuthority`, which is never exposed past the host boundary. Finding 2 (`derive_child` on an
observed value) — derivation is now a method on the authority, not the value; holding a copy of a
`Principal` grants nothing. Finding 3 (multi-hop) — `is_ancestor_of()` walks a real, internally-owned
table, so a grandchild's real ancestry is always answerable regardless of depth, without asking any
external caller to supply or vouch for a chain.

**Honestly named residual**: `IdentityAuthority` is now the one thing whose OWN access must be
tightly scoped by whatever integrates this design (§9) — it is exactly as security-critical as
`CapabilitySet::grant_root()` already is in the existing, non-reused system, and needs the same
"never reachable from anything derived from model output" discipline enforced at the integration
layer, not just asserted here.

### 13.2 `BranchHandle` resolution — a real synchronous fallback, not a discarded coroutine

```cpp
class BranchHandle {
public:
    BranchHandle(BranchHandle&&) noexcept;
    BranchHandle& operator=(BranchHandle&&) noexcept;
    BranchHandle(BranchHandle const&) = delete;
    BranchHandle& operator=(BranchHandle const&) = delete;

    // The destructor is now PURELY SYNCHRONOUS — it never tries to run abandon()'s real body. If
    // still unresolved at destruction, it does exactly one thing a destructor safely CAN do: push
    // this branch's name onto a lock-free, intrusive pending-abandon queue owned by the Ledger
    // (a plain atomic push, no coroutine, no lock that could itself require awaiting). This makes
    // the "never silently forgotten" property real without pretending a destructor can drive async
    // work: the branch's protection isn't lifted here, but it's also not left with NOTHING recording
    // that it needs resolving.
    ~BranchHandle();

    [[nodiscard]] std::string const& name() const;
    [[nodiscard]] Principal          created_by() const;
    [[nodiscard]] Digest             base_digest() const;
private:
    friend class Ledger;
    // ...
};

class Ledger {
public:
    // unchanged from Revision 1 except as noted:
    [[nodiscard]] task<result<Checkpoint>> merge(BranchHandle child, BranchHandle const& parent, MergeStrategy);
    [[nodiscard]] task<result<void>>       abandon(BranchHandle child);

    // NEW — the real driver for what the destructor above only QUEUES. A host must call this
    // periodically (the natural call site is the same turn-boundary loop that already drives
    // harvest_and_checkpoint(), or an explicit idle-time hook) — named and owned, not hand-waved as
    // "runs in the background." Each call drains the queue and genuinely co_awaits abandon() for
    // every pending name, so the coroutine body actually executes this time.
    [[nodiscard]] task<std::size_t> reap_pending_abandons();  // returns how many it processed, so a
                                                                // caller/test can observe real progress
                                                                // instead of trusting a fire-and-forget
                                                                // claim
private:
    // an internal, lock-free (or coarsely-mutexed, chosen at implementation time — this document
    // does not mandate lock-free specifically) queue of branch names pushed by BranchHandle's
    // destructor; reap_pending_abandons() is the only consumer.
};
```

`collect_garbage()` gets the identical treatment: it is no longer described as something that "runs
opportunistically... never blocks" on its own — it is a coroutine a real caller (the same turn-
boundary/idle driver as `reap_pending_abandons()`) explicitly `co_await`s. Nothing in this design
claims background execution without naming who actually drives it, closing this finding and the
identical one against `collect_garbage()` the same way.

**Honestly named residual**: a destructor-queued name that's never drained (host never calls
`reap_pending_abandons()`) is a real, disclosed leak-until-reap window — strictly better than
Revision 1's claim (which leaked with NO record at all), but not a claim of zero leak. This is the same
class of honest limitation `FileAppendLogStore`-shaped designs elsewhere in this project's own
engineering culture already accept for crash-adjacent edges (a torn trailing record, here a
never-drained queue entry) rather than something this design invents new laxity for.

### 13.3 `AsyncQuota<T>` — fully coroutine-native, with a real per-child ledger

```cpp
template <class Kind>
class AsyncQuota {
public:
    [[nodiscard]] static AsyncQuota mint_root(Principal owner, std::uint64_t total);

    // NOW a coroutine — closing the sync/async race directly: every mutator of remaining_/children_
    // takes the SAME mutex_, no exceptions.
    [[nodiscard]] task<result<AsyncQuota>> allocate_child_share(Principal child, std::uint64_t amount);

    [[nodiscard]] task<result<void>> try_consume(std::uint64_t amount, Principal spender);

    // Takes the amount explicitly and validates it against a REAL per-child ledger this type now
    // keeps — closing the no-anti-replay / no-amount-parameter finding directly: releasing more than
    // was ever allocated to `child`, or releasing a child that was never allocated a share, or
    // releasing the same child twice, all fail closed (quota.release_not_owed) rather than silently
    // crediting remaining_ upward.
    [[nodiscard]] task<result<void>> release_child_share(Principal child, std::uint64_t amount);

    [[nodiscard]] std::uint64_t remaining() const noexcept;   // still a plain, unlocked read — same
                                                                 // "read while nothing is concurrently
                                                                 // consuming" caller responsibility as
                                                                 // Revision 1, unchanged because this
                                                                 // specific tradeoff was never the
                                                                 // part round 1 found broken
private:
    Principal      owner_;
    std::uint64_t  remaining_;
    std::unordered_map<std::uint64_t /*child principal id*/, std::uint64_t /*amount allocated*/> children_;
    AsyncMutex     mutex_;
};
```

### 13.4 `Ledger::commit()` — storage quota becomes a required, structural parameter

```cpp
[[nodiscard]] task<result<Checkpoint>> commit(BranchHandle const& branch, Digest new_tree,
                                                Principal authored_by,
                                                AsyncQuota<StorageBytes>& quota);
    // Matches branch_from()'s own discipline exactly, closing the contradiction: there is no path to
    // a commit that doesn't pass a quota, the same way there was never a path to a branch_from() that
    // didn't. quota.try_consume() runs BEFORE the write; a rejected quota fails the commit closed
    // (the caller's real work up to the previous checkpoint is untouched, matching the historical
    // design's own fail-closed framing for this exact failure mode).
```

`Checkpoint` also gains the field needed to resolve the `reset_to`/`turn_index`/`parent` ambiguity:

```cpp
struct Checkpoint {
    Digest      self_digest;      // NEW — this checkpoint's own identity, so `parent` below has
                                   // something unambiguous to chain to
    Digest      tree;
    Digest      parent;           // now explicitly: the PRIOR CHECKPOINT'S self_digest, {} for a
                                   // branch's root checkpoint — never the tree digest
    Principal   authored_by;
    std::uint64_t turn_index;     // reset_to() ALWAYS assigns current_head_turn_index + 1 to the new
                                   // checkpoint it commits — never the restored target's own
                                   // turn_index — so turn_index stays strictly monotonic and unique
                                   // within a branch even across any number of rollbacks
};
```

### 13.5 `MediatedFileSystem` — a synchronous facade at the one boundary that must be synchronous

The coroutine-native framing in Revision 1's §6 overclaimed against a fact this design cannot change:
`Tool<Derived, Policies...>::invoke` is synchronous by this project's own deliberate, disclosed
choice, not an accident this design gets to route around. The fix is not to make tools async — it is
to give `MediatedFileSystem` two layers: an internal, genuinely async core (used by `SandboxSession`'s
own lifecycle operations — `materialize()`, `harvest_and_checkpoint()` — which run from the host's real
coroutine-based session loop, where `task<T>` is exactly the right shape) and a synchronous facade
that is the ONLY thing a `Tool<>` body ever sees:

```cpp
class MediatedFileSystem {
public:
    // The tool-reachable surface. Synchronous, matching Tool::invoke's real, permanent contract.
    // Internally, each call blocks on its own async core operation completing (a bounded, in-process
    // wait — not a network round trip — the same "synchronous facade over async internals" shape this
    // project's own EffectContext-adjacent code already uses in places, stated here as a pattern
    // this design follows because it's correct, not because it's borrowed).
    [[nodiscard]] result<std::vector<std::byte>> read(std::string const& path, Grant<FsRead> const&, Principal caller);
    [[nodiscard]] result<void> write(std::string const& path, std::vector<std::byte> data, Grant<FsWrite> const&, Principal caller);
    [[nodiscard]] result<std::vector<std::string>> list_dir(std::string const& path, Grant<FsRead> const&, Principal caller);

    // The session-lifecycle-only async core — never called from a Tool<> body, only from
    // SandboxSession's own coroutine methods.
    [[nodiscard]] task<result<std::vector<std::byte>>> read_async(...);
    [[nodiscard]] task<result<void>>                    write_async(...);
};
```

Also fixed here: how a `Grant<FsRead>`/`Grant<FsWrite>` reaches a tool call at all. Following the
REAL `ScheduleWakeupTool` shape (now correctly characterized, not the fictional "via `EffectContext`"
one from Revision 1): `RollbackAuthority`/`FsRead`/`FsWrite`-gated tools that need this design's
`Grant<T>` model are wired via a session-capturing dispatch closure (mirroring
`make_tool_descriptor_with_invoke`'s real, existing shape) that reads a `GrantSet` the session itself
holds — the identity-native analog of `CapabilitySet`, with the same encapsulated, `find_*`-shaped
lookup surface round 1's Finding 6 correctly said was missing:

```cpp
class GrantSet {
public:
    template <class Payload> [[nodiscard]] std::optional<Grant<Payload>> find(Principal caller) const;
    template <class Payload> [[nodiscard]] std::vector<Grant<Payload>> find_all(Principal caller) const;
private:
    // encapsulated storage — never a bag of public fields a caller could harvest, closing the same
    // hazard 13.1 closes for Grant<T> itself, applied to the collection that holds many of them.
};
```

A model-controlled `Args` field is never a `Grant<T>` and never becomes one — closing the I3 risk
Finding 7 named. `GrantSet` is populated only by host code holding an `IdentityAuthority`, the same
choke point as everything else in 13.1.

### 13.6 §6/§11 native-shell mediation — claim brought down to match the honest hedge, new attacks recorded

§6's "structurally impossible" language for native-process write mediation is replaced with: "closes
the ordinary case (a mediated read/write call) completely; does NOT yet have a proven answer for a
process that outlives its own call (double-fork/background) or a symlink created inside its own
scratch directory before this design's mediation layer resolves the target path — both real,
independently-found attack shapes, not merely theoretical, recorded honestly in §11 rather than
implied solved by §6's own prose." §11 is updated to name these two concretely rather than only the
previously-listed `mmap`-after-call case.

### What Revision 2 does NOT claim to have closed

Consistent with this design's own discipline: `IdentityAuthority`'s own access-scoping at integration
time (§9, still deferred, now with a sharper specific requirement than Revision 1 had); the storage
backend, merge-conflict algorithm, cross-session ceiling, and mediation-performance questions from
§11 (unchanged, still open); and — stated plainly — this revision has NOT yet been independently
red-teamed. Every one of Revision 1's "confident structural claim" failures came from asserting a fix
without adversarial verification; this document does not repeat that mistake by calling Revision 2
"done." A round 2 red-team against §13 specifically is the correct next step before any of this moves
toward implementation.

## 14. Round 2 red-team findings — a strategic pattern, not just new bugs

Two independent reviewers, run specifically against §13's claimed fixes. The individual findings
matter less than what they converge on: **building this stack fresh, with zero reused machinery, did
not avoid the failure classes the historical (machinery-reusing) design hit across four revisions —
it reproduced the identical three failure classes, with a different cast of types.** That is the real
result of this round, stated plainly rather than buried in a disposition table.

**The three recurring classes, present again here:**
1. **"Host-only" stated in a comment is not "host-only" enforced by the compiler.** `IdentityAuthority`
   has a fully `public`, argument-less constructor. Any code — not just hostile code, ordinary careless
   code — can construct a second instance and mint whatever `Principal`/`Grant<T>` it wants; `authorized()`
   takes a caller-supplied `IdentityAuthority const&` with nothing binding it to "the" real one, so a
   self-constructed authority's empty ancestry table trivially "confirms" anything asked of it. This is
   the exact same shape as Revision 1's `Principal::mint_root` bug, moved one layer down, not removed.
   `AsyncQuota<T>::mint_root` was never touched by Revision 2 at all and has the identical hole — a
   caller can mint a fresh, unlimited quota and hand it to `Ledger::commit()`'s new "required" parameter,
   defeating that fix's entire purpose without writing a single line the type system would reject.
2. **"Runs in the background" needs a real, named driver — asserting one exists is not designing one.**
   `reap_pending_abandons()`/`collect_garbage()` are mechanically correct NOW (they really do
   `co_await abandon()`'s real body, unlike Revision 1's discarded task) — but nothing in §6, §8, or §9
   ever calls them. §13.2's own text concedes this ("a host must call this... the natural call site is
   ... or an explicit idle-time hook" — two hypotheticals, neither wired into the one sequence diagram
   this document actually maintains). The BLOCKING finding was "no driver exists"; the fix supplies a
   method a driver *could* call, not a driver.
3. **A synchronous facade over async internals can deadlock this project's real, single-thread
   cooperative coroutine model, and "not a network call" doesn't address that.** `MediatedFileSystem`'s
   sync facade, as described, can only mean spinning `resume()`/`done()` on the async core's task. If
   that core ever contends an `AsyncMutex` also touched by the same thread's own outer session loop —
   plausible, since §13.5 places both "on the host's real coroutine-based session loop" — the parked
   waiter can only be resumed by an `unlock()` call from the lock's current holder, which may be the
   very thread that's now blocked spinning. `AsyncMutex` (verified directly) has no synchronous
   acquisition path, no thread pool, no out-of-band wake mechanism to break this. This is the same
   sync-over-async assumption that broke §4/§5 in round 1, now recurring at a third, independent site.

**New, self-inflicted defect found inside Revision 2's own fix**: §13.1's prose claims "every real
mutator on this type is now a coroutine," but the code block in the SAME subsection declares
`mint_root`/`derive_child`/`mint_grant` with plain synchronous return types, while `mutex_` is typed
`AsyncMutex` — which has no legal synchronous acquisition path at all. The fix written to close the
forgery finding reintroduced the sync/async mismatch bug class inside itself, undetected by its own
author (this session) before red-team caught it. `authorized()` also does not compile as written —
it calls `grant.issued_to_id()`, a method `Grant<T>` never declares (only `friend class
IdentityAuthority`, not `authorized`, was granted access).

**Real, partially-fixed items**: `release_child_share` gained the required `amount` parameter and a
per-child ledger, closing the "no bound at all" half of the original finding — but the document never
states whether a ledger entry is erased/zeroed on successful release, so a literal implementation can
still double-release the same child's share by re-checking an unchanged ledger entry.
`Checkpoint::self_digest`/`parent`/`turn_index` are correctly disambiguated in what they reference, but
`self_digest`'s own hash computation (exclude-self-from-input, the standard content-hash pattern) is
never spelled out — solvable, but unspecified, leaving room for two implementations to diverge.

**A previously-recorded finding was dropped, not fixed and not disclosed**: §12's §8-diagram
use-after-move finding (`BranchHandle` reused after `SandboxSession::create()` already moved it in) is
absent from both §13's fix list and its own honest-residuals list — worse than a named gap, since it
reads as forgotten rather than deferred.

**Confirmed genuinely closed, both reviewers agree**: the destructor-can't-drive-a-coroutine defect
(replaced with a synchronous queue push — real, if its own lifetime/allocation questions are still
open); the internal `AsyncQuota` race between `try_consume`/`allocate_child_share` (both coroutines
under the same mutex now); the `MediatedFileSystem`/`Tool::invoke` shape correction and the
`ScheduleWakeupTool` mischaracterization (now correctly described as a session-capturing dispatch
closure over ambient state, not a fictional `EffectContext`-carried `Grant<T>`).

## 15. Revision 3 — fixing root cause, not the fifth instance of the same three bugs

Two rounds found the SAME three failure classes recurring under different names. Revision 3 does not
add a fourth patch to a fourth symptom — it fixes each class once, structurally, and re-derives every
site in §13 that depended on the broken version.

### 15.1 Unforgeable minting — a real singleton, not a comment

```cpp
// The ONE IdentityAuthority instance that will ever exist in a given process. Enforced, not
// commented: private constructor, a single static bootstrap() that constructs it exactly once
// (throws/aborts on a second call — a loud, immediate failure at the one call site that could ever
// misuse this, not a silent bypass), and no copy/move — there is structurally one of these, or the
// program does not proceed.
class IdentityAuthority {
public:
    IdentityAuthority(IdentityAuthority const&) = delete;
    IdentityAuthority& operator=(IdentityAuthority const&) = delete;

    // Host process entry point calls this ONCE, at startup, before any session exists. A second call
    // anywhere in the process is a programming error, not a security boundary to be crossed
    // silently — it fails loudly (std::terminate-shaped, matching this project's own posture on
    // "this must never happen" contract violations elsewhere, e.g. SessionShellSandbox's "must never
    // move" discipline).
    [[nodiscard]] static IdentityAuthority& bootstrap();

    // Minting is genuinely synchronous — identity minting is a rare, host-driven, low-contention
    // operation (root sessions, forks, spawns — not a per-tool-call hot path), so a plain
    // std::mutex is the right tool, not AsyncMutex. This also fixes round 2's self-inflicted defect:
    // there is no more prose/code mismatch, because these were never coroutines to begin with.
    [[nodiscard]] Principal mint_root(std::string label);
    [[nodiscard]] Principal derive_child(Principal const& parent, std::string label);
    [[nodiscard]] bool is_ancestor_of(std::uint64_t candidate_ancestor_id, std::uint64_t descendant_id) const;
    template <class Payload>
    [[nodiscard]] Grant<Payload> mint_grant(Payload payload, Principal issued_to, Principal issued_by);

private:
    IdentityAuthority() = default;
    friend IdentityAuthority& bootstrap_impl();   // the ONLY constructor call site in the program
    std::unordered_map<std::uint64_t, AncestryRecord> ancestry_;
    std::atomic<std::uint64_t> next_id_{1};
    std::mutex mutex_;   // plain, synchronous — see above
};

// authorized() now takes NO caller-supplied authority parameter at all — it always consults
// IdentityAuthority::bootstrap()'s one real instance, closing the "pass in your own fake authority"
// attack at the type level: there is no parameter position left for a forged one to occupy.
template <class Payload>
[[nodiscard]] bool authorized(Grant<Payload> const& grant, Principal const& caller, Payload const& requested) {
    return IdentityAuthority::bootstrap().is_ancestor_of(grant.issued_to_id(), caller.id())
           && subsumes_payload(grant.payload(), requested);
}
```

`Grant<T>` gains `friend bool authorized<>(...)` explicitly (closing round 2's compile-error finding —
checked this time, not asserted).

**`AsyncQuota<T>::mint_root` gets the identical treatment** — the exact gap round 2 found untouched:

```cpp
// mint_root is now ALSO gated through IdentityAuthority — a quota can only be minted for a Principal
// IdentityAuthority itself vouches for as real (its own ancestry_ table), closing the "mint your own
// unlimited quota, hand it to commit()'s 'required' parameter" bypass directly. This is the same
// choke point as Principal/Grant minting, not a second, parallel one.
[[nodiscard]] static result<AsyncQuota> mint_root(IdentityAuthority const& authority, Principal owner,
                                                    std::uint64_t total);
    // fails closed (quota.unknown_principal) if `authority` (necessarily the one real instance, per
    // 15.1's own removal of any other way to obtain an IdentityAuthority reference) has no record of
    // `owner` ever having been minted.
```

Since `IdentityAuthority` cannot be constructed by anyone but its own single `bootstrap()` call, and
`mint_root` requires a live reference to it, there is no longer a parameter-substitution attack
available anywhere this pattern is used — the same fix closes `Principal`/`Grant<T>` forgery, the
`authorized()` bypass, and the `AsyncQuota::mint_root` bypass with one mechanism, not three.

### 15.2 A real, named driver — wired into the one diagram that matters, not left to hypothetical integration

```cpp
// A small, explicitly-scoped maintenance step — NOT a general executor/thread pool (this document
// does not invent new engine-wide concurrency machinery). It runs on the SAME thread and at the SAME
// call site as harvest_and_checkpoint() already does, once per turn boundary — the one real, existing
// "something runs after every turn" moment this design's own §8 sequence diagram already depends on.
class SandboxSession {
public:
    // ... unchanged from §6, plus:
    [[nodiscard]] task<result<Checkpoint>> harvest_and_checkpoint(Principal turn_owner);
        // NOW internally co_awaits ledger_.reap_pending_abandons() and ledger_.collect_garbage()
        // immediately after committing — same call, same turn boundary, no separate hook a host could
        // forget to wire in. This is the fix for round 2's "no driver exists anywhere in this
        // design's own text": there is now exactly one, and it's the same one thing every session
        // already calls every turn, not a second thing to remember.
};
```

§8's sequence diagram (below, superseding the Revision 1 diagram) reflects this directly rather than
describing it only in prose.

### 15.3 `MediatedFileSystem` — two locks that never meet, so a sync facade can never wait on itself

The deadlock round 2 found is a lock-sharing problem, not a "blocking is inherently unsafe" problem —
the fix is to make sure the synchronous, tool-body-reachable path and the asynchronous, session-loop
path never contend the SAME mutex.

```cpp
class MediatedFileSystem {
public:
    // Tool-reachable, synchronous. Touches ONLY staged_writes_ (below), guarded by a plain
    // std::mutex — never AsyncMutex, never anything the async core also locks. A tool-body call can
    // never block on something only the session's own coroutine loop could unblock, because the two
    // sides share no lock at all.
    [[nodiscard]] result<std::vector<std::byte>> read(std::string const& path, Grant<FsRead> const&, Principal caller);
    [[nodiscard]] result<void> write(std::string const& path, std::vector<std::byte> data, Grant<FsWrite> const&, Principal caller);
    [[nodiscard]] result<std::vector<std::string>> list_dir(std::string const& path, Grant<FsRead> const&, Principal caller);

    // Session-loop-only. Drains staged_writes_ under the SAME std::mutex (a quick, always-
    // terminating critical section — copy out and clear, never itself awaiting anything while held)
    // and THEN, lock released, feeds the result into the Ledger's own async commit path — the
    // AsyncMutex-guarded side never runs while the std::mutex is held, so there is no lock that is
    // ever needed by both a blocked synchronous caller and a coroutine only that same caller could
    // resume.
    [[nodiscard]] task<result<std::vector<StagedWrite>>> drain_staged_writes();

private:
    std::mutex mutex_;                        // guards staged_writes_ ONLY — plain, synchronous
    std::vector<StagedWrite> staged_writes_;
};
```

`harvest_and_checkpoint()` (§6/15.2) calls `drain_staged_writes()` (async, safe — it's the session's
own coroutine loop calling it, not a tool body blocking on it) and then `Ledger::commit()` with the
result — the async, `AsyncMutex`-guarded machinery stays entirely on the session-loop side, which is
where it was always safe.

### 15.4 Remaining round-2 items, closed directly

- **`release_child_share` anti-replay**: the ledger entry is explicitly erased on successful release
  (`children_.erase(child.id())`), stated as code now, not left to inference:
  ```cpp
  [[nodiscard]] task<result<void>> release_child_share(Principal child, std::uint64_t amount) {
      auto guard = co_await mutex_.lock();
      auto it = children_.find(child.id());
      if (it == children_.end() || it->second != amount) {
          co_return std::unexpected(error{failure_class::policy, "release does not match a live allocation",
                                            "quota.release_not_owed"});
      }
      children_.erase(it);        // <-- the explicit step round 2 found missing
      remaining_ += amount;
      co_return result<void>{};
  }
  ```
  A second `release_child_share(child, amount)` call now fails closed (`quota.release_not_owed`) since
  the entry no longer exists — literal double-release is closed by the code itself, not by prose
  asserting it.
- **`self_digest` computation, stated explicitly**: `self_digest = hash(tree || parent || authored_by
  || turn_index)` — computed by `Ledger::commit()` itself (never caller-supplied, closing the
  "who computes it" ambiguity), over exactly those four fields, `self_digest` excluded from its own
  input by construction (the standard content-hash pattern, stated rather than assumed).
- **The dropped §8 use-after-move finding, actually fixed this time**: `SandboxSession` gains
  ```cpp
  [[nodiscard]] BranchHandle release_branch() &&;   // rvalue-qualified — can only be called on a
                                                       // SandboxSession being torn down/consumed,
                                                       // never on a live one still in use
  ```
  and the updated §8 diagram (below) shows the host calling `release_branch()` before `merge()`/
  `abandon()`, so no diagram step references a variable already moved elsewhere.
- **`BranchHandle` destructor's remaining honest residuals (Ledger-lifetime, queue capacity)**: not
  closed by code — closed by explicit specification instead of silence: `Ledger` outlives every
  `BranchHandle` it issues BY CONTRACT (the same non-negotiable ordering this project already demands
  of `SessionShellSandbox`'s own "must never move" callers — a documented precondition, checked at
  integration time by whatever owns both objects' lifetimes, not proven by this document alone); the
  pending-abandon queue is a fixed-capacity ring buffer sized at `Ledger` construction, and an overflow
  (more unresolved handles dropped than capacity) is a disclosed, fail-loud condition
  (`ledger.abandon_queue_overflow`), never a silent drop — this document does not claim unbounded
  capacity, it names the bound and the failure mode explicitly.

### Updated §8 diagram, reflecting 15.2/15.4 directly

```mermaid
sequenceDiagram
    participant Host
    participant S as SandboxSession
    participant MFS as MediatedFileSystem
    participant L as Ledger
    participant Q as AsyncQuota<StorageBytes>
    participant IA as IdentityAuthority (bootstrap singleton)

    Host->>IA: bootstrap()  -- once, process startup, before any session
    Host->>IA: mint_root(label) -- root Principal
    Host->>L: create_root_branch(principal)
    L-->>Host: BranchHandle
    Host->>S: SandboxSession::create(ledger, handle, surface)

    loop each turn
        Host->>MFS: dispatch tool call -- write() (sync, std::mutex only)
        MFS->>MFS: stage write (never touches AsyncMutex)
        Host->>S: harvest_and_checkpoint(turn_owner)
        S->>MFS: drain_staged_writes() (async, session-loop side)
        S->>Q: try_consume(bytes_written, principal)
        S->>L: commit(branch, tree, principal, quota)
        S->>L: reap_pending_abandons()   -- same call, every turn, not a hypothetical hook
        S->>L: collect_garbage()
    end

    alt fork_from()
        Host->>IA: derive_child(parent_principal, label)
        Host->>Q: allocate_child_share(child_principal, amount)
        Host->>L: branch_from(parent_handle, child_principal, child_quota)
        L-->>Host: child BranchHandle
        Host->>S: SandboxSession::create(ledger, child_handle, surface)
    end

    alt branch concludes
        Host->>S: std::move(child_session).release_branch()
        S-->>Host: BranchHandle (never touched by create() again -- no use-after-move)
        Host->>L: merge(child_handle, parent_handle, strategy)  -- OR --
        Host->>L: abandon(child_handle)
        Host->>Q: release_child_share(child_principal, amount)
    end
```

## 16. Round 3 red-team findings — the crux question, and it exposed a threat-model error, not just new bugs

One combined reviewer, verifying §15 against §14. Verdict: **the pattern was not broken.** Two concrete
compile errors survived from round 2 unfixed (one — `grant.issued_to_id()` calling a method `Grant<T>`
never declared — flagged explicitly in round 2 and left untouched while §15's own text claimed it was
"checked this time, not asserted"). `release_branch() &&` fixes the literal diagrammed use-after-move
but leaves the `SandboxSession` object itself fully callable afterward — the identical "asserted
host-only/consumed, not enforced" texture recurring in object-lifecycle form. The two-lock
`MediatedFileSystem` split (§15.3) is confirmed to genuinely close the deadlock class — but checking it
against this project's REAL tool-dispatch code (`core/tool_pipeline.hpp`'s `background_task()`, which
dispatches a backgroundable tool's `invoke()` on a **detached `std::thread`**, not the session's own
coroutine thread) surfaced a real, undisclosed consequence: a backgrounded write can arrive after an
arbitrary number of later turns have already drained/committed, so `drain_staged_writes()`'s "whatever
is staged now belongs to the current turn" assumption silently misattributes it — a genuine, new I4 gap
this design hadn't previously reasoned about, found only because a reviewer checked the fix against
real production dispatch code instead of the document's own model of it.

**The crux finding, and why it's actually a threat-model error, not an unfixed bug**: the reviewer
confirmed `IdentityAuthority::bootstrap()` achieves object *singularity*, not access *control* —
`mint_root`/`derive_child`/`mint_grant` remain plain public methods any linked code can call once it
has the (now singular) reference, which `bootstrap()` hands out to anyone who asks. Read literally,
this looks like round 1/2's "host-only enforced by comment" bug recurring a third time. It is not,
once compared honestly against what this project's own REAL, already-shipped `CapabilitySet::
grant_root()` actually provides — re-read here rather than assumed: `trust/capability.hpp`'s own
header comment describes it as "the one explicitly-named, greppable entry point host policy calls,"
never as something compiler-enforced against arbitrary linked C++ code. Nothing stops a native
`Tool<Derived>::invoke()` body from calling `CapabilitySet::grant_root({cap::FsWrite{"/", ...}})`
itself and self-escalating, TODAY, in the shipped system — and this has never been treated as a gap,
because this project's actual I2/I3 threat model (`AgentEngineSpecification.md` §4, `CLAUDE.md`'s own
"host code never model output" framing for `ADR-070`) draws the trust line at **provenance of data
and code**, not at **which C++ symbols are link-reachable within one process image**: native `Tool<>`
bodies are host-authored, compiled C++ (002/006's "v1 authoring surfaces are C++ CRTP"), trusted the
same way any other first-party engine code is; the only real untrusted surfaces are model-emitted
DATA (`Args`, `TaintedText` — never compiled/linked code) and WASM guest code (genuinely
link-isolated — a WASM module cannot call an arbitrary host C++ symbol at all, only what the Component
Model ABI explicitly imports to it). Judged against that real, already-established bar — not the
stricter "safe against any linked native C++, including a host's own tool authors" bar the last three
rounds implicitly applied — `IdentityAuthority::bootstrap()` matching `grant_root()`'s exact posture is
correct, not a residual gap. **This is a genuine correction to how this design was being red-teamed,
not a discovered vulnerability**, and it resolves round 3's Finding 1(c) and Finding 3
(`AsyncQuota::mint_root`) as already-adequate rather than needing a fourth escalation of enforcement
that the rest of this project doesn't have either.

### What Revision 4 fixes for real, below: the two compile errors, the lifecycle-invalidation gap, and the background-thread attribution race

### What Revision 3 still does not claim

`bootstrap()`'s own call-site discipline (nothing yet stops a SECOND process-wide bootstrap attempt
from being reachable if integration wiring is careless — the fail-loud behavior is real, but "fails
loudly" is a runtime property, not a compile-time one, and this document does not claim otherwise);
every item already listed as open in §11; and, as always, this revision has not yet been red-teamed
independently. Given the pattern this document has now observed twice — a fresh mechanism looking
sound until adversarial review finds the same three failure classes recurring — a third round against
§15 specifically, before any implementation step, is not optional caution, it is this document's own
established, earned rate of finding real defects on every single pass so far.

## 17. Revision 4 — the two real compile errors, the release-flag gap, and the background-write attribution race

### 17.1 `Grant<T>` — read access via plain public accessors, no friend-template trickery needed

Round 3's `grant.issued_to_id()` compile error, and the fragile `friend bool authorized<>(...)`
declaration it was paired with, both existed to solve a problem that was never actually the security
property in question: **reading** an already-constructed `Grant<T>`'s `issued_to` was never the
forgery risk (`Checkpoint::authored_by` and `BranchHandle::created_by()` are public accessors for the
identical kind of value, unchallenged across three rounds) — only unrestricted *construction* was.
Fixing this by exposing plain, ordinary public accessors and keeping ONLY the constructor
friend-gated removes the need for the fragile template-friend syntax entirely:

```cpp
template <class Payload>
class Grant {
public:
    [[nodiscard]] Payload const& payload() const { return payload_; }
    [[nodiscard]] std::uint64_t issued_to_id() const { return issued_to_.id(); }
    [[nodiscard]] std::uint64_t issued_by_id() const { return issued_by_.id(); }
    [[nodiscard]] std::uint64_t grant_id() const { return grant_id_; }
private:
    friend class IdentityAuthority;     // ONLY the constructor needs gating
    Grant(Payload payload, Principal issued_to, Principal issued_by, std::uint64_t grant_id);
    Payload       payload_;
    Principal     issued_to_;
    Principal     issued_by_;
    std::uint64_t grant_id_;
};
```

`authorized()` (§15.1) is unchanged in behavior, now compiles as literally written, and needs no
special friend declaration on either side:

```cpp
template <class Payload>
[[nodiscard]] bool authorized(Grant<Payload> const& grant, Principal const& caller, Payload const& requested) {
    return IdentityAuthority::bootstrap().is_ancestor_of(grant.issued_to_id(), caller.id())
           && subsumes_payload(grant.payload(), requested);
}
```

### 17.2 `IdentityAuthority::is_known()` — the missing existence check `AsyncQuota::mint_root` needed

```cpp
class IdentityAuthority {
public:
    // ... unchanged, plus:
    [[nodiscard]] bool is_known(std::uint64_t principal_id) const;   // O(1) lookup against ancestry_ —
                                                                       // the exact "was this ever
                                                                       // actually minted" question
                                                                       // AsyncQuota::mint_root's own
                                                                       // fail-closed check needs and
                                                                       // round 3 found had no method
                                                                       // to call
};
```

`AsyncQuota<T>::mint_root`'s check becomes concrete: `if (!authority.is_known(owner.id())) return
std::unexpected(error{failure_class::policy, "principal was never minted", "quota.unknown_principal"});`

### 17.3 `SandboxSession::release_branch()` — a real, runtime-enforced released flag, not prose

```cpp
class SandboxSession {
public:
    [[nodiscard]] BranchHandle release_branch() &&;

    // EVERY other method now begins with the identical guard — released_ is checked, not asserted:
    [[nodiscard]] task<result<void>> materialize() {
        if (released_) return std::unexpected(error{failure_class::contract,
            "SandboxSession used after release_branch()", "sandbox_session.already_released"});
        // ...
    }
    // harvest_and_checkpoint(), reset_to_turn(), filesystem() all gain the identical first line.

private:
    bool released_ = false;   // set true, synchronously, inside release_branch() itself, before the
                               // extracted BranchHandle is returned to the caller — there is no window
                               // where release_branch() has returned but released_ is still false
};
```

This closes round 3's finding for real: calling any other method on a `SandboxSession` after
`release_branch()` now fails closed with a real error code, checkable by a test, not merely
discouraged by a doc comment.

### 17.4 Background-write attribution — per-write authorship survives independently of which turn drains it

The real gap round 3 found (`core/tool_pipeline.hpp`'s `background_task()` dispatches on a detached
`std::thread`, so a backgrounded tool's write can arrive turns after it was issued) is fixed by not
relying on `Checkpoint::authored_by` to carry per-write attribution at all — it never should have:

```cpp
struct StagedWrite {
    std::string   path;
    std::vector<std::byte> bytes;
    Principal     author;      // ALREADY available at write() time (its own `Principal caller`
                                // parameter) — round 3's gap was that this was implicitly discarded
                                // by the time drain_staged_writes() ran, not that it was unavailable
};

struct Checkpoint {
    Digest        self_digest;
    Digest        tree;
    Digest        parent;
    Principal     authored_by;    // now explicitly documented as "who TRIGGERED this checkpoint" (the
                                   // turn owner / drain caller) — never claimed to be sole author of
                                   // every byte in it
    std::uint64_t turn_index;
};

// Tree entries now carry their own attribution, independent of which Checkpoint eventually commits
// them — a background write's REAL author survives being drained by a later, different turn's
// harvest, closing the I4 gap at the granularity it's actually needed:
struct TreeEntry {
    std::string path;
    Digest      blob;
    Principal   written_by;   // NEW — per-entry, never overwritten by whichever turn's
                               // harvest_and_checkpoint() happened to be the one that drained it
};
```

**Honestly named residual, not smoothed over**: this preserves WHO actually wrote a given byte range
regardless of WHEN it gets committed — it does not, and cannot, retroactively move a late-arriving
background write into the turn that logically issued it (that turn may have already committed and
moved on by the time the detached thread's write lands). A checkpoint's `turn_index`/`authored_by`
therefore describes "the turn during which this commit happened to occur," not "the turn during which
every byte in it was produced" — a real, disclosed distinction, not an equivalence this design gets to
assume away.

**Content-addressing interaction, decided explicitly (round 4 found this genuinely unresolved)**:
`written_by` is sidecar metadata on a `TreeEntry`, NEVER part of what a tree's own content digest is
computed over — only `path`/`blob` participate in tree-level hashing. This preserves tree-level
sharing/dedup exactly as before (two branches that reach byte-for-byte-identical state still produce
the identical tree digest regardless of which principals wrote which files along the way); the cost,
stated plainly rather than glossed over, is that `written_by` is NOT itself covered by the tree's own
content-addressed integrity guarantee — a tampered `written_by` value on an otherwise-correct tree
would not change that tree's digest. This is an accepted tradeoff (attribution metadata riding beside
a content-addressed structure, not woven into it), not an oversight — but it means anything that later
needs to trust `written_by` for something security-relevant needs its own integrity story, which this
document does not currently provide and names as a real, disclosed gap rather than assuming solved.

### 17.5 A session that never runs a turn never drives `reap_pending_abandons()`/`collect_garbage()` — named, not fixed

Round 3's observation stands as a real, narrow residual: these are `Ledger`-global operations, driven
by ANY session's `harvest_and_checkpoint()` call — as long as at least one session in a deployment
completes at least one turn, the whole `Ledger` gets maintained (this is NOT scoped per-session, so a
busy deployment's turns maintain even a completely idle sibling session's pending abandons). The
disclosed edge case that remains is a deployment where NO session ever completes a single turn, which
is already a degenerate state for reasons well beyond garbage collection — named here rather than
silently left for a future reader to rediscover.

## 18. Round 4 red-team findings — the threat-model correction holds, with real caveats; three concrete gaps in §17

One reviewer, tasked specifically with trying to break §16's own threat-model argument rather than
accept it, plus verifying §17's four fixes. Verdict on the correction: **directionally right, but
asserted with more confidence than the evidence fully earns.** Grepping every real `grant_root()` call
site in the repo confirms the convention (host/policy setup, or narrowing an already-held set via
`attenuate()`) is real and consistently followed — but found ZERO mechanical enforcement anywhere (no
lint rule, no CI check, `naming_lint.py` checks vocabulary names only) — §16 should say "an unenforced,
precedented convention," not imply an established, compiler-backed boundary. More substantively:
`grant_root()`'s real call sites are exclusively either one-time setup with a hardcoded/config list or
pure narrowing (never minting new, unconstrained authority) — `IdentityAuthority::mint_root()`/
`derive_child()` as designed would be called far more frequently, from more diverse call sites
(`fork_from()`, `agent.spawn()`), minting genuinely NEW identity each time with no "already covered by
what the caller holds" constraint analogous to `attenuate()`'s. §16 never engaged with this
call-frequency/kind disanalogy — it is real, and worth naming rather than treating the analogy as
fully closing the question. The WASM-import-surface half of §16's argument (a guest module cannot
reach an arbitrary host symbol) was independently verified against the real `wasm_backend.cpp` import
classifier and holds up completely as implemented, not just as claimed.

**Real, unaddressed gap in §17.2**: `Principal::id_` was originally specified (§3) as "process-random,
unforgeable"; the real Revision 3/4 implementation (`IdentityAuthority::next_id_`, §15.1) is a
sequential `std::atomic<std::uint64_t>` counter. Combined with the new `is_known()` (§17.2) and the
already-existing `is_ancestor_of()`, any code holding the `IdentityAuthority&` reference (which, per
§16's own corrected threat model, is any native host-authored code) can trivially enumerate
`is_known(1), is_known(2), ...` and walk the entire live ancestry table. Under the corrected threat
model this isn't a boundary violation (native code was already trusted) but it is a silent, undisclosed
regression from the original design's own stated rationale for why `id_` was random in the first place
— never reconciled anywhere in this document.

**Real, unaddressed gap in §17.3**: `released_` is a plain, unsynchronized `bool`, and
`SandboxSession`'s other methods are coroutines with real internal `co_await` suspension points. A
genuine TOCTOU race exists: a method passes the `released_` check, suspends mid-body at an internal
`co_await`, and while suspended, a DIFFERENT call path does `std::move(session).release_branch()` —
nothing stops this (the rvalue-qualification only gates which overload a call resolves to, it enforces
no exclusivity), and the first coroutine resumes afterward still holding a reference to a
`BranchHandle` that has since been extracted out from under it. §17.3's fix closes only the literal
"call another method after `release_branch()` has already returned" case from §8's diagram — not
concurrent-with-in-flight-coroutine misuse.

**Real, unaddressed gap in §17.4**: the document never states whether `TreeEntry::written_by`
participates in a tree's own content-hash — if it does, tree-level dedup breaks (identical file states
authored by different principals no longer share a tree digest); if it doesn't, `written_by` rides
outside the content-addressed integrity guarantee entirely. Left genuinely unresolved by Revision 4.

## 19. Revision 5 — closing round 4's three concrete gaps; the threat-model caveat is now stated, not overclaimed

### 19.1 §16, restated with the honesty round 4 found missing

Replace §16's confidence with what the evidence actually supports: `IdentityAuthority::bootstrap()`
matching `grant_root()`'s posture is a legitimate application of this project's real, established
provenance-based trust line (`ADR-070`/`ADR-071`, the verified-closed WASM import surface) — but it is
an **unenforced, precedented convention**, not a compiler-backed boundary, exactly as `grant_root()`
itself is; and it is being asked to do MORE work than `grant_root()` ever has in practice (frequent,
diverse-call-site, unconstrained-new-identity minting, vs. `grant_root()`'s real one-time-setup/
narrowing-only usage) — a real difference in exposure this document now names rather than treats as
already absorbed by the analogy. **Consequence, not just caveat**: whoever integrates this design (§9)
should treat "who is allowed to call `IdentityAuthority`'s minting methods" as a real code-review/lint
surface worth actually building (the same way `naming_lint.py` already exists for a different
convention) rather than inheriting `grant_root()`'s current zero-enforcement status by default — this
document recommends, but does not itself build, a lint rule restricting `IdentityAuthority::mint_root`/
`derive_child`/`mint_grant`/`AsyncQuota::mint_root` call sites to an explicitly allow-listed set of
host/orchestration files, closing the enforcement gap `grant_root()` itself still has today.

### 19.2 `Principal::id_` — disclosed as sequential, `is_known()`'s enumeration cost named plainly

The original "process-random" framing (§3) is retired as inaccurate to what actually shipped in §15.1
— stated here rather than left as a quiet contradiction. `next_id_` stays a sequential counter (a
random 128-bit id would need its own collision/generation story this document doesn't need to solve
just to fix an enumeration concern that the corrected threat model already accepts as low-severity):
**`is_known()`'s enumerability is an accepted, disclosed cost of a trusted-native-code threat model,
not a residual vulnerability** — under §16/§19.1's own corrected model, code that could enumerate
`ancestry_` was already trusted code capable of calling `mint_root`/`derive_child` directly, so learning
which ids exist grants it nothing it didn't already have access to mint for itself.

### 19.3 `SandboxSession` — real exclusivity, matching I1 instead of asserting it

The fix is to make `SandboxSession` actually obey this project's own I1 ("a session's... state is
mutated by at most one executor at any instant") rather than assume the rvalue-qualification on
`release_branch()` provides it for free:

```cpp
class SandboxSession {
public:
    // release_branch() is now a coroutine, taking the SAME internal exclusivity lock every other
    // method takes — genuinely serialized with materialize()/harvest_and_checkpoint()/reset_to_turn(),
    // not merely gated by which overload a call syntactically resolves to.
    [[nodiscard]] task<result<BranchHandle>> release_branch() &&;

    [[nodiscard]] task<result<void>> materialize() {
        auto guard = co_await exclusivity_.lock();
        if (released_) return std::unexpected(error{failure_class::contract,
            "SandboxSession used after release_branch()", "sandbox_session.already_released"});
        // ... method body runs to completion still holding guard, or re-derives its own narrower
        // critical section internally — the specific internal shape is an implementation choice; the
        // REQUIREMENT this document now states is that no two SandboxSession methods, including
        // release_branch() itself, may have overlapping critical sections that read/write released_
        // or the owned BranchHandle.
    }
private:
    // Held behind unique_ptr, not by value -- §21.1's prove-phase finding (found in AsyncQuota<T>,
    // fixed here pre-emptively before this type was itself probed): AsyncMutex is non-copyable AND
    // non-movable (LockAwaiter stores a raw pointer to it), so a by-value AsyncMutex member would make
    // SandboxSession itself unreturnable from SandboxSession::create()'s task<result<SandboxSession>>
    // (§6) -- the exact same incompatibility AsyncQuota<T> had, caught here by inspection instead of a
    // second expensive compile failure.
    std::unique_ptr<AsyncMutex> exclusivity_;   // the SAME primitive AgentSession's own I1 discipline
                               // already relies on elsewhere in this project — reused here because
                               // it's the correct tool for exactly this job, not because this document
                               // is relaxing its no-reuse framing; a session-scoped mutual-exclusion
                               // lock is infrastructure, not one of the four primitives this design
                               // set out to replace
    bool released_ = false;
};
```

This closes the TOCTOU gap round 4 found: `release_branch()` cannot run concurrently with a suspended
`materialize()`/`harvest_and_checkpoint()` call, because both now serialize through the same
`exclusivity_` lock — the property was always supposed to hold (I1 is a project-wide invariant, not
something this design gets to opt out of for one type), it just wasn't actually implemented before.

### 19.4 What Revision 5 still does not claim

The lint-rule recommendation in §19.1 is a recommendation, not a built artifact — this document still
does not implement enforcement, it names the gap and proposes closing it as real follow-on work, honestly
distinguishing "we recommend X" from "X now exists." Everything in §11's original open-questions list
remains open. This is the fifth revision of a document whose every prior revision was independently
red-teamed and found wanting at least once — Revision 5 is not exempt from that pattern by virtue of
being the most recent; another round against §19 specifically remains the correct next step before any
of this is treated as ready for a real compile-probe/prove phase.

## 20. Prove phase, part 1 — `IdentityAuthority`/`Principal`/`Grant<T>`, real compiler evidence

Per this project's own established discipline (a real MSVC compile failure is what actually proved
ADR-096's C2 property — not a design document asserting it), the mechanism from §15.1/§17.1/§17.2 was
implemented standalone (no dependency on the real `agentengine` tree, matching this design's no-reuse
framing) and run against a real compiler: **clang 22.1.5, target `x86_64-pc-windows-msvc`, `-std=c++23`**.
Source under `docs/planning/proofs/identity_authority/`.

**A real, unresolved contradiction from round 3 was settled by writing the code, not by more prose**:
§15.1's own text describes `bootstrap()` as something that "throws/aborts on a second call," while the
rest of the design calls it repeatedly (`authorized()` on every check, `derive_child()` on every fork).
Those two statements cannot both be true. Implementing it forces the choice: **`bootstrap()` is a
Meyer's singleton accessor — safe to call any number of times, thread-safe by C++11's magic-statics
guarantee, always returning the same instance.** The design doc's "aborts on a second call" framing is
retired as simply wrong, corrected here by what the real call sites actually need, exactly the kind of
error a compile probe exists to catch that a prose review already missed twice.

### 20.1 Positive probe — `probe_positive.cpp`, 9 checks, real runtime, all pass

```
[1] bootstrap() singularity: PASS (repeated calls return the same instance)
[2] mint_root(): PASS (id=1 known=1)
[3] direct child ancestry: PASS
[4] MULTI-HOP grandchild ancestry: PASS (this is the property Revision 1's single-parent_id
    Principal could NOT provide)
[5] three-hop ancestry: PASS
[6] sibling non-ancestry: PASS
[7] is_known() rejects an unminted id: PASS
[8] authorized() end-to-end (incl. multi-hop descendant inheritance, and correctly rejecting a
    genuinely unrelated principal): PASS
[9] Grant<T> public read accessors: PASS

ALL CHECKS PASSED
```

Check [4]/[5] are the real proof that round 1 Finding 3 (multi-hop descent — a grandchild via
`fork_from()`-of-`fork_from()` could not be represented by Revision 1's single-`parent_id` `Principal`)
is actually fixed: `IdentityAuthority`'s internal `ancestry_` table is walked to full depth, verified
three hops deep, not merely claimed.

**A real defect the compiler+runtime caught, disclosed rather than quietly fixed**: the first version
of check [8] asserted `!authorized(grant, sibling, ...)` — expecting a direct child of the grant's
`root` principal to be REJECTED. Running it failed immediately (`CHECK FAILED at probe_positive.cpp:74`).
On inspection this was a test-authoring mistake, not an implementation defect: `sibling` really is a
child of `root` and is correctly authorized to use `root`'s grant per this design's own inheritance
rule (§3/§13.1: a descendant may exercise a grant issued to an ancestor). The negative case was
rewritten to use a genuinely unrelated principal (`stranger`, an independent second root), which is
correctly rejected. Recorded here because this is exactly the discipline this whole document has tried
to hold itself to: a real tool caught a real mistake immediately, and the mistake is disclosed rather
than silently corrected out of the historical record.

### 20.2 Negative probes — four compile-fail claims, all verified against real compiler output

| Probe | Claim being tested | Result |
|---|---|---|
| `probe_negative_principal_construct.cpp` | A `Principal` cannot be hand-constructed outside `IdentityAuthority` (closes round 1 Finding 1/2's forgery path) | **Confirmed fails to compile**: `error: calling a private constructor of class 'probe::Principal'`, at the exact construction line, `note: declared private here` pointing at `identity_authority.hpp:41` |
| `probe_negative_authority_construct.cpp` | `IdentityAuthority` cannot be constructed except via `bootstrap()` (closes round 2/3's "anyone can construct their own authority" finding) | **Confirmed fails to compile**: `error: calling a private constructor of class 'probe::IdentityAuthority'`, `note: declared private here` at `identity_authority.hpp:132` |
| `probe_negative_authority_copy.cpp` | `IdentityAuthority` is non-copyable (the C2-shaped requirement from §9) | **Confirmed fails to compile**: `error: call to deleted constructor of 'probe::IdentityAuthority'`, `note: ... has been explicitly marked deleted here` at `identity_authority.hpp:77` |
| `probe_negative_grant_construct.cpp` | A `Grant<T>` cannot be hand-constructed naming an arbitrary `issued_to`, even by code that already holds real `Principal` values (round 1 Finding 1's literal attack, replayed against the fix) | **Confirmed fails to compile**: `error: calling a private constructor of class 'probe::Grant<Dummy>'`, `note: declared private here` at `identity_authority.hpp:60` |

### 20.3 What this prove phase does and does not establish

**Established, empirically, for the first time in this document's history**: the singleton/friend/
access-control shape in §15.1/§17.1/§17.2 is not merely asserted to compile and enforce what it
claims — it does, verified against a real C++23 compiler, both for the intended-success paths (9/9)
and the intended-failure paths (4/4). The multi-hop ancestry property specifically — the one piece of
Revision 1 that was structurally impossible to provide — is now real, tested code, not prose.

**NOT established by this pass, named plainly**: concurrent/multi-threaded correctness of `mint_root`/
`derive_child`/`mint_grant` under real contention (this probe is single-threaded); the `Ledger`,
`AsyncQuota<T>`, `SandboxSession`/`MediatedFileSystem` primitives from §4-§6/§13.3-§13.5/§15.2-§15.3
(none of them implemented or probed yet — this pass covers `IdentityAuthority`/`Principal`/`Grant<T>`
only, per the scope this prove phase was asked to start with); §19.1's recommended lint-rule
enforcement (still unbuilt); and round 4's own caveat that this whole mechanism is a "trusted native
code" boundary, not a cryptographic one — a real compile probe proves the ACCESS-CONTROL shape works
as specified, it does not and cannot prove the underlying THREAT-MODEL judgment from §16/§19.1 is the
right one for a given deployment; that remains a design/policy call, not something a compiler checks.

## 21. Prove phase, part 2 — `AsyncQuota<T>`, and a real, previously-undiscovered systemic finding

Source: `docs/planning/proofs/async_quota/`. Same compiler (clang 22.1.5, `-std=c++23`), this time
including the REAL `agentengine::rt::task<T>`/`agentengine::rt::AsyncMutex` headers directly (not
reinvented — these are pre-existing coroutine SUBSTRATE the whole engine already runs on, orthogonal
to the Capability/Worktree/Sandbox machinery this design's no-reuse framing is actually about).

### 21.1 A real design flaw no textual red-team round found, caught on the first compile attempt

`agentengine::rt::AsyncMutex` deletes its copy constructor/assignment and declares no move members —
per C++'s own rule, a user-declared special member function (a deleted one counts) suppresses the
implicitly-generated move constructor too. **`AsyncMutex` is therefore non-copyable AND non-movable,
deliberately**: `LockAwaiter::await_suspend` stores a raw `self` pointer to the specific mutex
instance a parked coroutine will later be resumed through — relocating a live `AsyncMutex` would
dangle that pointer for any coroutine currently queued against it. This is a real, correct, load-
bearing property of the actual project code, verified by reading `async_mutex.hpp` directly — not a
bug to work around, a constraint to respect.

**Consequence, missed across every prior round because nobody had tried to actually build the type**:
`AsyncQuota<T>` as specified in §13.3 embeds `AsyncMutex mutex_` **by value**, and `mint_root()`/
`allocate_child_share()` both return a fresh `AsyncQuota` **by value**. These two facts are flatly
incompatible — a type containing a by-value `AsyncMutex` can never itself be returned by value, moved,
or stored in any container that might relocate its elements. The real compiler caught this immediately
(`error: call to deleted constructor of 'std::expected<AsyncQuota<...>, error>'`, `note: copy
constructor of 'AsyncQuota<...>' is implicitly deleted because field 'mutex_' has a deleted copy
constructor`) — not a subtle template error, a hard, first-attempt compile failure.

**§15's own `SandboxSession::exclusivity_` fix (§19.3) has the IDENTICAL defect**, discovered here by
inspection before it was even probed: `SandboxSession` also embeds `AsyncMutex exclusivity_` by value,
and `SandboxSession::create()` (§6) returns a fresh `SandboxSession` **by value** from a
`task<result<SandboxSession>>`. The exact same class of bug recurs in a second primitive this design
specified — matching this whole document's own now five-times-observed pattern (every fix round has
found, or in this case a prove round found before the fix round even started, at least one new real
gap) — corrected in §22 below, before that primitive's own probe was built, rather than discovering it
the same expensive way twice.

**The fix, applied and verified**: hold the mutex behind a stable-address indirection —
`std::unique_ptr<agentengine::rt::AsyncMutex> mutex_` instead of a by-value member. Moving a
`unique_ptr` relocates the pointer, never the pointee, so the real `AsyncMutex` object's memory address
never changes regardless of how many times the OWNING type (`AsyncQuota`/`SandboxSession`) is moved —
closing the incompatibility while preserving every other part of the design (`mint_root`/
`allocate_child_share` still return by value; callers see no API change).

### 21.2 Positive probe — 7 checks, real single-threaded correctness + real multi-threaded contention, all pass

```
[1] mint_root() with a real, known principal: PASS (remaining=1000)
[2] try_consume(200): PASS (remaining=800)
[3] try_consume() over-budget rejection: PASS
[4] allocate_child_share(300): PASS (parent remaining=500, child quota remaining=300)
[5] release_child_share() first call: PASS (remaining=800)
[6] ANTI-REPLAY: second release_child_share() of the same allocation: REJECTED (remaining stayed
    at 800, not double-credited)
[7] concurrency test: 10 threads x 2000 iters -- total_consumed=20000, alloc/release cycles=20000,
    remaining()=980000, expected=980000
ALL CHECKS PASSED
```

Check [6] is the real proof of the Revision 5 (§15.4/§19) anti-replay fix: the ledger entry is
genuinely erased on a successful release, so a second `release_child_share()` call with the same
`(child, amount)` fails closed (`quota.release_not_owed`) rather than double-crediting `remaining_` —
confirmed against real code, not asserted in prose this time.

Check [7] is the real proof of the Revision 3 fix (§13.3) — the exact scenario round 1's security
reviewer worked through by hand (a concurrent guarded `try_consume` and an unguarded
`allocate_child_share` interleaving to overspend a 1000-unit root by 100 units) run for real: **10
real OS threads, 2000 iterations each (20,000 total `try_consume`/`allocate_child_share`/
`release_child_share` cycles), against one shared `AsyncQuota` and its one `AsyncMutex`**. `remaining()`
reconciled EXACTLY with total consumption on every one of 5 repeated runs (100,000 lock acquisitions
total) — no lost or double-counted budget. The original race (allocate_child_share unguarded, racing a
guarded try_consume) is empirically closed under real contention, not merely argued to be closed on
paper.

### 21.3 What this prove phase does and does not establish

**Established**: the Revision 3 sync/async race fix and the Revision 5 anti-replay fix both hold under
real, repeated, multi-threaded execution against the real `AsyncMutex`; and a real, previously-
undiscovered incompatibility between "embeds an `AsyncMutex` by value" and "returns itself by value"
was found and fixed, with the identical defect pre-emptively caught and corrected in `SandboxSession`
before it could repeat the same discovery cost.

**NOT established at the time this section was written**: `AsyncQuota<T>`'s interaction with
`Ledger::commit()`'s required-parameter binding (§13.4) — **since closed** by §23/§27's real
`Ledger::commit()`, which takes and consumes a real `AsyncQuota<StorageBytes>&` on every call; this
note is left here for the historical record rather than silently deleted, per this document's own
practice of correcting rather than erasing stale claims (see also §34.10's own such correction).
The `is_known()` gate in `mint_root()` is, by inspection,
currently unreachable dead code under this design's own rules (the only way to obtain ANY `Principal`
object at all is via the one `IdentityAuthority` singleton, which registers it in `ancestry_`
immediately — there is no serialization/reconstruction path that could produce a `Principal` unknown
to it), named honestly here as a harmless but currently-untestable defense-in-depth check rather than
claimed as a meaningfully exercised one.

## 22. Prove phase, part 3 — `SandboxSession` exclusivity and `MediatedFileSystem`'s two-lock split

Source: `docs/planning/proofs/sandbox_session/`. Same compiler and real `rt::task<T>`/
`rt::AsyncMutex` headers as §21. §19.3's `exclusivity_` member was fixed to `unique_ptr<AsyncMutex>`
in this document (§19.3's code block, above) BEFORE this probe was written, applying §21.1's finding
pre-emptively rather than rediscovering it a second time.

### 22.1 Real concurrent race: `materialize()` vs. `release_branch()` — the round 4 TOCTOU, closed

20 real trials, each spawning two genuine `std::thread`s racing `materialize()` (holding the lock
150ms, simulating real async session work) against `release_branch()`, varying the start-time gap
between them to produce both possible real orderings:

```
[1] 20 real concurrent materialize()/release_branch() races: PASS (18 resolved as
    materialize-first, 2 resolved as release-first; critical sections NEVER overlapped, and every
    release-first materialize() call was correctly rejected, not silently ignored)
[2] materialize() after a completed release_branch(): REJECTED (sandbox_session.already_released)
```

**A second test-authoring mistake, caught and disclosed the same way §20's was**: the first version of
this probe asserted every trial must produce exactly 4 log events (materialize fully running, then
release fully running). Running it failed roughly 1 trial in 3 (`log.events.size() == 2`, not 4). On
inspection this was, again, a flawed test expectation, not a design defect: when `release_branch()`
genuinely wins the race, `materialize()` — correctly serialized behind the same lock — observes
`released_ == true` the instant it acquires the lock and fails closed BEFORE logging anything,
producing 2 events, not 4. This is the exclusivity fix working exactly as intended, not a bug; the test
was rewritten to accept both valid orderings and assert the RIGHT thing for each (no overlap in the
4-event case; a genuine rejection, not a silent no-op, in the 2-event case). Two real mistakes now
caught by two different probes in this prove phase, both disclosed rather than smoothed over — the
same discipline this document has tried to hold every red-team round to, now holding itself to it
during implementation too.

### 22.2 The deadlock round 2/3 warned about, reproduced for real — then closed for real

`probe_deadlock_demo.cpp` deliberately builds the exact antipattern round 2/3's reviewers predicted
(a synchronous facade blocking, via a real `block_on()`, on an async operation that needs the SAME
`AsyncMutex` the calling thread already holds) and proves it self-deadlocks — not argued, reproduced:

```
[1] Sharing ONE AsyncMutex between a sync-blocking wait and a critical section the SAME call chain
    already holds: CONFIRMED SELF-DEADLOCK (inner operation did not complete within 5,000,000
    spins).
```

`probe_two_lock_safe.cpp` then builds §15.3's actual fix — a `MediatedFileSystem`-shaped type with a
plain `std::mutex` guarding `staged_writes_` for the tool-reachable sync `write()` path, and a
completely separate `AsyncMutex` (`commit_lock_`) for the session-loop-only async
`drain_staged_writes()` path — and proves the deadlock class genuinely cannot occur because the two
paths never share a lock:

```
[1] sync write() while a slow (300ms) async drain holds the SEPARATE commit lock: PASS (write()
    completed in 0ms, not ~300ms -- no lock sharing, no deadlock)
[2] copy-and-clear boundary: PASS (no write lost, none duplicated, no stale re-delivery)
[3] concurrent writers/drainer: total_written=4000, total_drained=4000
[3] CONCURRENT sync write() from 8 real threads racing a real async drain loop: PASS (every write
    drained exactly once, none lost, none duplicated)
```

Check [1] is the direct before/after pair with §22.2's deadlock demo: the identical shape of
contention (a sync caller vs. a slow, lock-holding async operation), but because `write()` never
touches `commit_lock_` at all, it completes in ~0ms regardless of a 300ms commit in flight on the
other lock — empirically confirming §15.3's central claim, not merely its absence of a *known*
counter-example. Check [3] is real, sustained multi-threaded load (8 writer threads × 500 writes each,
racing a continuously-draining thread) reconciling exactly (4000 written, 4000 drained, zero lost,
zero duplicated) — the copy-and-clear boundary genuinely holds under real contention, not just the
single-writer case §22.2 spot-checked.

### 22.3 What this prove phase does and does not establish

**Established**: §19.3's exclusivity fix genuinely closes round 4's TOCTOU under real, repeated,
varied-timing concurrent execution; the deadlock class round 2/3 predicted for a naive shared-lock
sync facade is real and reproducible, not a theoretical worry; and §15.3's two-lock split is a real,
verified fix for it — proven both by a direct timing comparison against the deadlock demo and by
sustained multi-threaded load with an exact reconciliation.

**NOT established**: this probe's `write()`/`drain_staged_writes()` are simplified stand-ins (no real
`Grant<FsWrite>` capability check, no real path-mediation, no real host filesystem I/O) — the two-lock
SHAPE is proven sound, not a claim that the full `MediatedFileSystem` surface from §6/§15.3 is complete;
`SandboxSession`'s other methods (`harvest_and_checkpoint()`, `reset_to_turn()`, `filesystem()`) were
not individually probed, only `materialize()`/`release_branch()`'s mutual exclusion; and the `Ledger`
primitive itself (§4/§13.2/§17) remains unprobed, next.

## 23. Prove phase, part 4 — `Ledger`, and the fourth-primitive close-out

Source: `docs/planning/proofs/ledger/`. Same compiler, real `rt::task<T>`, and the already-proven
`IdentityAuthority`/`AsyncQuota<T>` from §20/§21 included directly (not reinvented — this is the first
probe that composes multiple already-probed primitives together, the way the real design's `Ledger`
methods genuinely take `AsyncQuota<BranchCost>&`/`AsyncQuota<StorageBytes>&` parameters). Digest is a
plain FNV-1a hex-string stand-in, not a real cryptographic primitive — the design itself never commits
to a specific algorithm (§11), so this is a deliberate, disclosed simplification sufficient to test
WELL-DEFINEDNESS, not collision-resistance. `MergeStrategy`/real three-way merge semantics remain
explicitly undesigned (§11) and are NOT implemented here — `merge()` is a trivial stub proving only the
consumes-by-value/resolution mechanics `abandon()` already needed, never claimed as real merge
correctness.

### 23.1 Two more real defects caught only by writing the code, both fixed, both disclosed

**`BranchState`/`Checkpoint` cannot be default-constructed, and `std::unordered_map::operator[]`
silently requires that.** Both structs hold a `Principal` field, and `Principal` deliberately has no
default constructor (13.1's own identity-unforgeability design: a `Principal` only ever comes from
`IdentityAuthority`). The first draft of this probe wrote `branches_[name] = BranchState{...}` and
`state.checkpoints[turn] = cp` — both fail to compile, because `operator[]` on an
`unordered_map` value-initializes a DEFAULT instance before assigning over it, and neither type has
one. Fixed by switching to `insert_or_assign()` everywhere, which never default-constructs. This is a
small, mechanical fix, but a real one a textual review would very plausibly have missed — it only
surfaces when code actually using the identity primitive's own non-default-constructibility (a
DELIBERATE property from §13.1) is written against ordinary standard-library container idioms.

**A default member initializer that silently minted a spurious "uninitialized" `Principal` on every
`BranchHandle` construction.** An early draft gave `BranchHandle::created_by_` a default-member-
initializer calling `IdentityAuthority::bootstrap().mint_root("uninitialized")` as a defensive habit —
harmless in that both real constructors already override it, but genuinely wasteful (every
`BranchHandle` construction would have minted and discarded an extra `Principal`, permanently growing
`IdentityAuthority`'s `ancestry_` table with garbage entries, once §21's own disclosed enumeration cost
is considered) and confusing. Removed — both real constructors already explicitly initialize the field,
so no default is needed at all.

### 23.2 Positive probe — 10 checks, all pass

```
[1] create_root_branch(): PASS
[2] commit() + self_digest recomputation matches exactly: PASS
[3] second commit chains parent correctly, distinct self_digest: PASS
[4] identical (tree,parent,authored_by) at different turn_index -> DIFFERENT self_digest: PASS
[5] reset_to(target=1) from head=2: PASS (new turn_index=3, restored tree matches checkpoint 1,
    history not overwritten)
[6] branch_from(): PASS (base_digest matches parent's head at branch time)
[7] abandon() consumes the handle, branch genuinely removed, no residual queue entry: PASS
[8] a BranchHandle dropped without merge()/abandon(): destructor queued a pending abandon
    (queue size=1) -- purely synchronous, no coroutine attempted
[9] reap_pending_abandons(): PASS (processed=1, queue now empty -- the queued branch's abandon()
    body genuinely ran)
[10] merge() consumes the handle, child branch removed, no residual queue entry: PASS
ALL CHECKS PASSED
```

Checks [2]-[4] are the real proof of §17.4's `self_digest`/`turn_index` fix: recomputing the digest
independently from a checkpoint's own fields matches exactly (well-defined, not ambiguous); the SAME
`(tree, parent, authored_by)` at two different `turn_index` values produces two DIFFERENT digests
(confirming `turn_index` genuinely participates in the hash, closing round 2's collision concern);
check [5] is the real proof of the "always `head+1`, never the restored target's own `turn_index`"
rule — `reset_to(target=1)` from a head of 2 produces turn 3, not 1, while still genuinely restoring
checkpoint 1's tree content.

Checks [7]-[9] are the real proof of §13.2's central claim, end to end: a `BranchHandle` dropped
without explicit resolution genuinely queues (check [8], a plain synchronous push — the destructor
never attempts to touch a coroutine at all, closing round 1/2's "discarded task, zero lines run"
defect at its root), and `reap_pending_abandons()` genuinely drains that queue and runs the real
`abandon()` body to completion (check [9], `processed == 1`, queue empties) — not asserted, executed.
Check [10] confirms `merge()` follows the identical by-value-consumption discipline as `abandon()`.

### 23.3 Negative probe — real compiler-enforced protection against accidental double-use

```
probe_negative_abandon_no_move.cpp:19:46: error: call to deleted constructor of 'BranchHandle'
note: 'BranchHandle' has been explicitly marked deleted here
```

Calling `ledger.abandon(root)` on an lvalue, with no explicit `std::move`, fails to compile — a real,
compiler-enforced barrier against accidentally resolving a branch while still holding what looks like
a usable handle. A caller must write `std::move(...)` at the call site: an explicit, grep-able
admission of giving up ownership, not something that can happen by a careless one-line call.

### 23.4 What this prove phase does and does not establish — and the four-primitive close-out

**Established**: `BranchHandle`'s move-only shape, its destructor's real synchronous-only queue push,
`reap_pending_abandons()`'s real draining of the real `abandon()` body, `self_digest`/`turn_index`'s
well-definedness including the specific collision case round 2 raised, and the compiler-enforced
no-accidental-lvalue-resolve property — all verified against real, compiling, running code, not prose.

**NOT established**: real content-addressed tree/blob storage (this probe's "tree" is an opaque
string, never diffed or partially shared); any real merge algorithm (explicitly out of scope, §11);
`collect_garbage()`'s real reachability-based eviction (not implemented in this probe — only the
destructor-queue/reap mechanism was tested); and multi-threaded contention on `Ledger`'s own internal
state (this probe's `Ledger` uses a single coarse `std::mutex` for simplicity — real concurrent
`commit()`/`branch_from()` calls from multiple sessions were not stress-tested the way §21's
`AsyncQuota` was).

**Four-primitive close-out**: every one of this design's four core primitives (`IdentityAuthority`/
`Principal`/`Grant<T>` — §20; `AsyncQuota<T>` — §21; `SandboxSession`/`MediatedFileSystem` — §22;
`Ledger` — this section) has now had its central, most-contested claims verified against a real
compiler and, where concurrency was the actual claim under test, real multi-threaded execution — not
merely re-argued in prose a sixth time. Across all four prove passes, **five real, previously-
undiscovered defects surfaced purely from attempting to write compiling code** (the `authorized()`/
`Grant<T>` friend-declaration errors already caught before this phase started in §17-19, the
`AsyncMutex`-embedded-by-value non-movability bug found in §21 and pre-emptively fixed in
`SandboxSession` before its own probe, and the two container-idiom/default-construction bugs found in
§23.1) — none of them were found by four full rounds of adversarial textual red-team, which is itself
the clearest evidence yet for why this project's own `design → red-team → prove → judge` discipline
insists on a real `prove` stage and does not treat exhaustive red-teaming as a substitute for it.

**What remains before any of this is `judge`-ready**: the honest residuals named throughout §20-§23
(concurrent `IdentityAuthority` minting under real contention; `Ledger`'s own internal concurrency;
the `is_known()` dead-code observation; real tree/blob storage; a real merge algorithm;
`IdentityAuthority`'s "trusted native code, not cryptographic" threat model per §16/§19.1, which no
compile probe can settle); and the full integration story into `AgentSession`/`ContextProvider`/
`Tool<>` that §9 has deliberately deferred throughout. This document has not attempted either in this
prove phase, consistent with its own scope.

## 24. Prove phase, part 5 — real integration against the live `ContextProvider`/`ChatClient` pipeline

Source: `docs/planning/proofs/integration/`. This is the first probe in this design's prove phase that
goes beyond the coroutine substrate and touches real, shipped, model-facing production machinery:
`core/context_provider.hpp`, `core/composed_context_provider.hpp`, `core/context_assembly.hpp`,
`core/tool.hpp`, `core/tool_pipeline.hpp`, `core/effect_context.hpp`, `core/json_value.hpp`,
`trust/principal.hpp`, `trust/capability.hpp` — all read in full and coded against exactly, not
guessed. Answers the concrete question that motivated it: what does a `ChatClient` actually receive
once this design's own identity/grant machinery is the thing deciding whether a tool is offered at
all.

### 24.1 `SandboxReflector` — a real `ContextProvider`, built on this design's own primitives

Mirrors the REAL, already-shipped `SandboxToolProvider` (`src/backends/native_jail/
sandbox_tool_provider.hpp:92-150`) shape exactly — a `ContextProvider` conformer holding a
sandbox-shaped object, contributing a tool descriptor via `make_tool_descriptor_with_invoke()`'s
session-capturing-closure pattern — but the tool it contributes, `reset_sandbox`, is authorized against
this design's own `Grant<RollbackAuthority>`/`IdentityAuthority` (§15.1/§17.2), not the real
`CapabilitySet`. Declares **no static `Capabilities<...>` ceiling at all** on `ResetSandboxTool` —
verified directly against the real `ScheduleWakeupTool` this design has cited as precedent throughout
(§7/§13.5/§15.1): `struct ResetSandboxTool : agentengine::Tool<ResetSandboxTool>`, same bare shape,
same reason (a compile-time ceiling can't express a live `max_turns_back` value).

### 24.2 What a `ChatClient` actually receives — real output, not a mockup

Running the real pipeline (`ComposedContextProvider<SandboxReflector>::on_context()` →
`assemble_context()` → the exact `ContextContribution` `AgentSession::run_rounds()` folds into
`ChatRequest{messages, tools}` at `agent_session.hpp:2127`) produces:

```
tools[0].name          = "reset_sandbox"
tools[0].description   = "Roll this session's sandbox back to an earlier checkpoint. ..."
tools[0].capability_ceiling.size() = 0   (empty -- matches ScheduleWakeupTool's real precedent)
tools[0].args_schema_json  = {"type":"object","properties":{"turns_back":{"type":"integer"}},"required":["turns_back"]}
tools[0].reply_schema_json = {"type":"object","properties":{"ok":{"type":"boolean"},"message":{"type":"string"}},"required":["ok","message"]}
tools[0].captures_session_state = true
tools[0].attribution.contributor_type = "sandbox_reflector"
```

The JSON schema fields are not hand-written — they come from the REAL `AE_JSON_SCHEMA`
reflection macro applied to this design's own `ResetSandboxArgs`/`ResetSandboxReply` structs and the
real `Tool<>::args_schema()`/`reply_schema()` machinery, run for real. **This is, verbatim, what would
appear in a real `ChatRequest.tools` entry a real `ChatClientT::chat`/`chat_stream`/`call` receives** —
the exact question this prove phase was started to answer.

A real invocation through the tool's own real closure (`tool.invoke(args_json, ctx)`, matching
`tool_pipeline.hpp`'s own `invoke_tool()` step shape — JSON in, JSON out) with `turns_back=3` against a
grant capped at `max_turns_back=5` succeeds (`{"ok":true,"message":"rolled back 3 turn(s)..."}`); the
same call with `turns_back=999` is correctly rejected (`sandbox.rollback_exceeds_grant`) — the dynamic
check genuinely runs and genuinely gates the outcome, not merely declared.

**Composition confirmed real, not special-cased**: composing `SandboxReflector` alongside an
unrelated second `ContextProvider` (`TrivialInstructionsProvider`) into one real
`ComposedContextProvider<TrivialInstructionsProvider, SandboxReflector>` produces a correctly merged
`ContextContribution` — `instructions` from provider 0, `tools` from provider 1 — via the real
`assemble_context()` union/attribution logic (`context_assembly.hpp:228-244`), confirming this
design's own §9 principle (an ordinary fan-out member, no coupling) holds against the real pipeline,
not just in the abstract.

A session whose sandbox has no execution surface contributes `tools.size() == 0` — the exact "no
`run_shell`-equivalent tool when there's nothing to run" rule from §2, verified against the real
pipeline rather than only asserted about it.

### 24.3 A real, previously undiscussed integration friction point — two incompatible `Principal` types

Building this probe required constructing a real `agentengine::EffectContext`, whose `principal` field
is the REAL, existing `agentengine::Principal` (`trust/principal.hpp:27-56`) — a **string-keyed**
identity (`id`, `tenant_id`, `kind`, `on_behalf_of`, `delegation_depth`) — not this design's own
`probe::Principal` (an opaque `uint64_t` minted by `IdentityAuthority`, §3/§15.1). These are two
genuinely different, incompatible identity representations, and this design never previously had to
reconcile them because every prior prove pass (§20-§23) was standalone. `SandboxReflector` works
around this by capturing its OWN `probe::Principal` (the session's owner, minted once at construction)
rather than deriving one from `ctx.principal` at all — the real `EffectContext::principal` is
constructed and passed through the probe but never actually consulted for authorization.

**This is a real, disclosed integration decision this design has not made**: a genuine production
integration needs to decide whether this design's `Principal`/`IdentityAuthority` REPLACES
`agentengine::Principal` as the engine's one identity representation (a large, invasive change — 007/
018's whole capability/trust model is keyed on the existing type), or the two are bridged (e.g.
`IdentityAuthority` keyed by `agentengine::Principal::id` — a string — instead of minting its own
parallel `uint64_t` space), or kept genuinely separate with an explicit translation layer at exactly
the boundary this probe found (`EffectContext` construction). This document does not resolve it here —
naming it is the honest result of this prove pass, not a defect to silently patch over.

**An unexpected, validating cross-check**: reading the real `agentengine::Principal` in full revealed
that its own `on_behalf_of` field is ALSO single-hop only — "names only the immediate parent's
`id`, not the full chain... 018 §7 G4, named out of scope" (`trust/principal.hpp:33-38`), the *exact*
limitation round 1's red-team (§12 Finding 3) found and required this design to fix via a real,
internally-owned multi-hop ancestry table (§15.1). The real, already-shipped project has not solved
this problem for its own identity type either — it disclosed the same narrowing this design's
Revision-1 attempt originally had, and left it out of scope. This does not excuse Revision 1's gap
(round 1 was right that a design claiming to close this exact class of hole should not reintroduce it
silently) — but it is worth recording plainly that the multi-hop ancestry property this design's
`IdentityAuthority` now provides is a genuine improvement over what the CURRENT, PRODUCTION identity
primitive offers, not merely parity with an already-solved problem.

### 24.4 What this prove phase does and does not establish

**Established**: this design's `ContextProvider`-facing surface (a reflector wrapping
`Grant<T>`/`IdentityAuthority`-based dynamic authorization) compiles and runs correctly against the
REAL, current `ContextProvider` concept, `ComposedContextProvider`, `assemble_context()`,
`ToolDescriptor`, `Tool<>`, and `EffectContext` — with zero changes needed to any of those real,
shipped files. The `ScheduleWakeupTool`-precedent pattern (§7/§13.5/§15.1's "no static
`Capabilities<>`, dynamic check in a session-capturing closure") is now proven, not merely cited, as
the right integration shape for this design's own new authority kinds. What a `ChatClient` actually
receives for a tool gated by this design's machinery is now known precisely, from a real run, not
inferred.

**NOT established**: the `Principal` reconciliation question named in §24.3 (left open, not decided);
real `SandboxSession`/`Ledger` wiring behind the reflector (this probe used `SandboxStandIn`, a
minimal stand-in exposing only `has_execution_surface()`/`authorize_reset()` — the real `harvest_and_
checkpoint()`/turn-boundary commit wiring from §6/§15.2 was not exercised here); how `GrantSet`
(§13.5/§15.3's own still-unimplemented type) would actually be populated and threaded to a
session-capturing closure in a real multi-tool, multi-session deployment (this probe hand-constructed
a single `Grant<RollbackAuthority>` directly); and whether `EffectContext` should eventually gain a
real field for this design's primitives (the way `sandbox_fs` was added for the existing
`FileSystemAdapter`) or whether the "never touches `EffectContext` at all, capture everything in the
closure" pattern this probe used is the long-term right answer — both are real, live options this
prove phase surfaces without picking one.

## 25. Prove phase, part 6 — `GrantSet` and the `Principal`-bridging answer to §24.3

Source: `docs/planning/proofs/grant_set/`, plus `identity_authority.hpp`'s additive
`IdentityAuthority::adopt()` method and `integration/sandbox_reflector.hpp`'s updated closure (both
regression-checked against every prior probe — full suite re-run, 6/6 probe sets green). This pass
answers both items §24.4 left open: how `GrantSet` is actually populated/looked-up in a multi-grant,
multi-session shape, and how the real `agentengine::Principal` (string-keyed, `trust/principal.hpp`)
reconciles with this design's own `IdentityAuthority`-minted `Principal` (`uint64_t`-keyed).

### 25.1 `GrantSet` — the real enumeration surface round 1 Finding 6 asked for

A type-erased (via `std::any`/`std::type_index`), per-`Payload`-kind container mirroring the real
`CapabilitySet`'s own proven `find`/`find_all` shape (`trust/capability.hpp`'s `find_fs_read`/
`fs_read_grants`/etc.) — never exposing a raw bag of grants, only typed lookups that go through the
real `IdentityAuthority::is_ancestor_of()` check. 5 checks, all pass:

```
[1] heterogeneous storage (RollbackAuthority + BranchCost in one GrantSet): PASS
[2] GrantSet::find() resolves multi-hop (grandchild finds a root-issued grant): PASS
[3] GrantSet::find() correctly returns nothing for an unrelated principal: PASS
[4] find_all() returns ALL matching grants (2), not just the first: PASS
[5] two independent GrantSets (simulating two sessions) do NOT leak into each other: PASS
```

Check [2] is the real proof that `GrantSet`'s lookup surface (not just `authorized()` called directly,
already proven in §20) correctly threads through the full multi-hop ancestry chain end to end — the
exact enumeration gap round 1 Finding 6 named as missing from Revision 1's design is now real, working
code. Check [5] confirms session isolation: two `GrantSet`s sharing the same process-wide
`IdentityAuthority` singleton (an unavoidable consequence of §16/§19.1's own threat model) do not
leak grants into each other — isolation is a property of which `GrantSet` object is consulted, not of
the singleton itself.

### 25.2 `IdentityAuthority::adopt()` — bridging the real, existing `agentengine::Principal`

Reading `trust/principal.hpp` in full (done for this pass, not before) settled the reconciliation
question §24.3 left open: `agentengine::Principal` is string-keyed (`id`, `tenant_id`, `kind`,
`on_behalf_of`, `delegation_depth`) and, notably, **has no private constructor and no unforgeability
guarantee of its own** — it relies on the identical provenance-based trust line this design's own §16/
§19.1 already settled on ("the host is trusted," `trust/principal.hpp:58`'s own comment for the
Embedded row). Bridging by string id therefore introduces no new attack surface under the
already-accepted threat model: code that could forge an `agentengine::Principal` could already call
`IdentityAuthority::bootstrap().mint_root()` directly.

`adopt(real_id, real_on_behalf_of)` is additive (no change to any previously-proven method) and
idempotent — the SAME real id always maps to the SAME internal `Principal`:

```
[6] IdentityAuthority::adopt() is idempotent: PASS (same real id -> same internal Principal)
[7] adopt() preserves real on_behalf_of ancestry when the parent was adopted first: PASS
[8] DISCLOSED LIMITATION confirmed: adopting a child before its real parent has ever been adopted
    registers it as an independent root -- ancestry is NOT retroactively established when the
    parent is adopted later.
```

Check [8] is a real, disclosed limitation of bridging by observation order, proven rather than assumed
— named honestly rather than smoothed over, matching this document's own established discipline.

**A validating cross-check from reading the real type, worth restating precisely**: `agentengine::
Principal::on_behalf_of` (`trust/principal.hpp:33-38`) is itself single-hop only — "names only the
immediate parent's `id`, not the full chain... named out of scope" — the exact limitation round 1
required this design to fix. `IdentityAuthority`'s real, internally-owned ancestry table (§15.1) is
therefore a genuine capability improvement over what ships in production today, not mere parity with
an already-solved problem, confirmed now by directly reading the code rather than inferring it.

### 25.3 The integration loop, closed for real

`sandbox_reflector.hpp`'s closure (§24) was updated to derive the caller's identity from the REAL
`ctx.principal` via `adopt()` at INVOCATION time, and to look up authorization through a real
`GrantSet` rather than a single captured `Grant<RollbackAuthority>`. Re-running the full integration
probe (§24) plus two new checks:

```
[4] A DIFFERENT real agentengine::Principal ('a-completely-different-user') invoking the SAME tool
    descriptor: REJECTED (no rollback grant held for this session) -- confirms the dynamic check
    really re-derives identity from ctx.principal per call via adopt(), not from a value captured
    once at contribution time
[5] A REAL delegated principal (agentengine::derive_on_behalf_of("probe-user",
    "delegated-sub-agent")) correctly INHERITS the grant through the bridge's ancestry recognition:
    {"ok":true,"message":"rolled back 2 turn(s) ..."}
```

Check [4] closes the exact gap §24.3 named ("the check runs against `owning_principal_`, captured...
not derived from `ctx` at all") — the check now genuinely depends on which real principal is present
in `ctx` at call time, proven by showing a different one is rejected. Check [5] proves the bridge
correctly recognizes a REAL delegation chain (`agentengine::derive_on_behalf_of`, the actual production
mechanism 007 §2 defines for sub-agent calls) as inheriting the grant — the two identity systems now
interoperate for the one concrete case this design's `reset_sandbox` tool needs, end to end, verified
against real production code on both sides of the bridge.

### 25.4 What this prove phase does and does not establish

**Established**: `GrantSet`'s enumeration surface works correctly, including multi-hop lookup and
session isolation; `IdentityAuthority::adopt()` is a real, working, idempotent bridge from the
production `agentengine::Principal` to this design's own identity system, correctly recognizing real
delegation chains when the parent has been observed first; and §24.3's disclosed integration gap (the
reflector's authorization check not actually depending on the real, live `ctx.principal`) is now
closed by real, re-verified code, not merely proposed.

**NOT established**: whether `adopt()`'s "parent must be observed first" limitation (check [8]) is
acceptable for a real deployment, or needs a different bridging strategy (e.g. eagerly adopting every
principal encountered anywhere, or changing `agentengine::Principal` itself to carry a stable
opaque id this design could key on directly) — a real product/architecture decision, not a compile
probe's to make; multi-threaded contention on `IdentityAuthority::adopt()`'s new `adopted_` map (not
stress-tested the way §21's `AsyncQuota` was); and everything else already named open in §11/§24.4
(real `SandboxSession`/`Ledger` wiring behind the reflector, the `EffectContext`-field question, real
tree/blob storage, a real merge algorithm).

## 26. Prove phase, part 7 — the full stack, composed and driven end to end

Source: `docs/planning/proofs/full_stack/`. Two design decisions §25.4 left open were made explicitly
here (author's own judgment, per project-owner direction to choose the best candidate rather than
leave every question open indefinitely), then proven, not merely asserted:

- **`EffectContext` gets no new field.** The session-capturing-closure pattern (§13.5/§15.1,
  proven working end to end in §24/§25) is adopted as the PERMANENT delivery mechanism for this
  design's own authority kinds — consistent with `ScheduleWakeupTool`'s real precedent, and avoiding
  any invasive change to a struct the whole engine already depends on.
- **`adopt()`'s "parent observed first" constraint is accepted, not re-engineered**, because it
  matches how real delegation actually happens: `agentengine::derive_on_behalf_of()` structurally
  requires the caller to already hold the real parent `Principal` object, so "parent adopted before
  child" is the natural order of real usage, not an awkward extra requirement. The mitigation is
  procedural: a host adopts a session's owning principal ONCE at session construction, not lazily.

The main event of this pass: does composing all four already-proven primitives into ONE real,
movable `SandboxSession` — the thing every prior prove-phase section deferred — actually work, or does
integration surface a fifth new seam the way it has every single time before in this document's
history?

### 26.1 One more instance of the §21.1 pattern, this time caught by inspection before compiling

Building the real `MediatedFileSystem` INTO `SandboxSession` (rather than standing alone, as in §22)
required composing it into a type that itself needs to be returned by value from
`SandboxSession::create()`'s `task<result<SandboxSession>>`. `MediatedFileSystem`'s `sync_mutex_` was
a by-value `std::mutex` in §22's original probe — and `std::mutex`, like `agentengine::rt::AsyncMutex`,
deletes its copy constructor and declares no move members, making it **equally non-movable** by the
identical C++ rule §21.1 found. This is the THIRD independent instance of "a type embeds a
non-movable synchronization primitive by value, then needs to be returned by value from a factory" in
this document's prove phase (`AsyncQuota` — found by a real compile failure; `SandboxSession::
exclusivity_` — found by inspection before its own probe; `MediatedFileSystem::sync_mutex_` — found by
inspection here, before compiling, applying the now-established pattern proactively). Fixed identically:
`std::unique_ptr<std::mutex>` instead of a by-value member. The probe then compiled cleanly on the
first attempt — the pattern-recognition held, and no fourth real compile failure was needed to catch
it this time.

### 26.2 The real `SandboxSession` — composing Ledger, MediatedFileSystem, and AsyncQuota into one type

```cpp
class SandboxSession {
    // create() returns SandboxSession BY VALUE from a coroutine -- possible now that every
    // synchronization primitive inside it (its own exclusivity_, and MediatedFileSystem's two
    // internal mutexes) is held behind a stable-address unique_ptr, not embedded directly.
    static task<result<SandboxSession>> create(Ledger&, BranchHandle);
    MediatedFileSystem& filesystem();
    task<result<Checkpoint>> harvest_and_checkpoint(Principal turn_owner, AsyncQuota<StorageBytes>&);
    task<result<Checkpoint>> reset_to_turn(std::uint64_t turn_index, Principal requested_by);
    task<result<BranchHandle>> release_branch() &&;
};
```

`harvest_and_checkpoint()` is, for the first time in this document, the REAL §6/§15.2 sequence
end to end in one function: drain staged writes → commit through the real `Ledger` (consuming the
real `AsyncQuota<StorageBytes>`) → run the real `reap_pending_abandons()` maintenance step, all inside
the same exclusivity-guarded critical section §19.3 proved race-free.

### 26.3 Driven through two real turns and a real rollback — all checks pass, first run

```
[1] Turn 1: real write + real on_turn_end() -> real Ledger.commit(): completed
[2] Turn 2: second real write + real on_turn_end() -> real Ledger.commit(): completed
[3] REAL reset_sandbox invocation (turns_back=1): {"ok":true,"message":"REAL Ledger checkpoint
    restored: tree=\"turn1/a.txt;\", new turn_index=3"}
[4] REAL reset_sandbox invocation with turns_back=999 (exceeds grant): REJECTED (requested
    rollback exceeds the grant's max_turns_back)
ALL CHECKS PASSED
```

This is not a stub reply this time: `reset_sandbox`'s real closure calls `SandboxSession::
reset_to_turn()`, which calls the real `Ledger::reset_to()` proven in §23, and the RESULT — a real
`Checkpoint` — is what the tool's JSON reply reports. Check [3] confirms two real, load-bearing
properties simultaneously, from one real run: the restored `tree` is genuinely turn 1's content
(`"turn1/a.txt;"`, not turn 2's `"turn2/b.txt;"` — the rollback actually rolled back, not a no-op), and
`turn_index=3`, not `1` — §17.4/§23's "always monotonically increasing, never the restored target's
own index" rule holds under real, multi-turn, multi-primitive composition, not just in §23's own
narrower single-primitive probe. Check [4] confirms the `GrantSet`/`IdentityAuthority` authorization
gate from §25 still holds correctly once real `Ledger` state is actually on the other side of it, not
a stand-in.

Re-run three times for stability (real coroutine composition across five primitives is exactly the
kind of thing worth checking isn't order-dependent luck) — identical output every time. The full
prove-phase regression suite (all 7 probe directories: `identity_authority`, `async_quota`,
`sandbox_session`, `ledger`, `grant_set`, `integration`, `full_stack`) was re-run together as a final
check — every probe set green.

### 26.4 What this prove phase does and does not establish

**Established**: the four core primitives this design's prove phase spent §20-§23 proving in isolation
compose into one real, working `SandboxSession` with no further mechanism changes needed beyond the
already-anticipated `unique_ptr` fix (§21.1's pattern, now applied a third time); a real `ChatClient`-
facing tool (`reset_sandbox`) can perform a genuine, checkpoint-accurate rollback through this
composed stack, gated by this design's own identity/grant system, driven through real turn boundaries
exactly as a real `AgentSession` would drive them (`ComposedContextProvider::on_context()`/
`on_turn_end()`, unmodified real production code). This is the most complete single piece of evidence
this document has produced that the overall architecture — not just its individual pieces — is sound.

**NOT established, still**: real content-addressed tree/blob storage (this probe's "tree" remains a
deterministic string concatenation, §11/§23's own disclosed scope limit); a real merge algorithm
(`MergeStrategy` remains explicitly undesigned); real host filesystem I/O behind `MediatedFileSystem`
(writes are staged in memory, never touch a real sandboxed directory); concurrent multi-session load
on one shared `Ledger`/`IdentityAuthority` (this probe is single-session, single-threaded, matching
its goal of proving COMPOSITION correctness, not re-proving concurrency already covered per-primitive
in §21/§22); and the `adopt()` "parent observed first" design decision's real-world adequacy, which
remains a disclosed, accepted tradeoff rather than something this prove phase could validate either
way.

**SUPERSEDED by §32**: this section's own Ledger (`../ledger/ledger.hpp`) and §29's hardened Ledger
(`../worktree_io/worktree_ledger.hpp`) diverged into two non-overlapping artifacts — a real,
independent code review caught this, and it is fixed for real in §32, which rewires this exact
`full_stack/` demo onto the one, now-unified Ledger. Read §32 before trusting this section's claims
about what the composed stack does or does not resist.

## 27. Prove phase, part 8 — real worktree content-addressing and real host filesystem I/O

Source: `docs/planning/proofs/worktree_io/`. Closes the two items §26.4 named as still not
established. A deliberate reuse decision, made now that the prove phase is well past pure design:
this pass uses the REAL `agentengine::WorktreeObjectStore` concept, `InMemoryWorktreeObjectStore`, and
`agentengine::compute_digest` (actual SHA-256 via Windows CNG/BCrypt, `src/core/worktree_digest.cpp`,
compiled and linked as a genuine second translation unit — not reimplemented, not stubbed) — but
deliberately does NOT reuse `agentengine::Mount`/`mount_read`/`mount_write`/`cap::FsRead`/`cap::FsWrite`
(`src/backends/native_jail/worktree_mount_sync.hpp`), because those are wired to the OLD `Capability`/
`CapabilitySet` system this design's own `Grant<T>`/`IdentityAuthority` exists to replace. The line
drawn: pure content-addressing (digest/blob/tree storage) has no capability-system entanglement and is
safe to reuse; anything capability-GATED stays on this design's own `GrantSet`, never blending the two
authorization systems at the one seam (real I/O) where the distinction actually matters most.

### 27.1 Real SHA-256, verified against the actual linked implementation, not assumed

Before building the probe, `agentengine::compute_digest` was compiled and linked standalone and
checked against a known answer: `compute_digest("hello")` produced
`2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824` — the correct, independently
verifiable SHA-256 of the ASCII string "hello". This is the same discipline §20's own probe used for
`is_ancestor_of`/`authorized()` (a known-answer check, not merely internal self-consistency).

### 27.2 `Ledger` re-based onto the real object store; `MediatedFileSystem` re-based onto real disk I/O

`Ledger` (§23's own FNV-1a `Digest` stand-in replaced) now owns a real
`agentengine::InMemoryWorktreeObjectStore` directly, and `commit()` takes a real `agentengine::Tree`
— `put_tree()`'s own real digest, sort, and dedup logic runs for every commit, not a caller-supplied
opaque string. `RealIoFileSystem` (replacing §22's in-memory `staged_writes_` vector for this pass)
writes real bytes to a real host directory via `std::filesystem`/`std::ofstream`, and — critically —
`drain_into_tree()` reads each touched path back **off real disk**, not from the in-process bytes
`write()` was originally given, closing the one gap an in-memory stand-in could otherwise hide (a
write that silently never reached disk would still look "written" to an in-memory record; it cannot
hide from a real re-read).

### 27.3 Real, end-to-end results — all pass, first run

```
Real host directory: C:\Users\...\Temp\ae_worktree_io_probe
[1] wrote real file a.txt = "hello world" -- confirmed on real disk
[2] REAL SHA-256 tree digest (turn 1)  = e851a6df2a600e24cdf29ebbe141fd9b1ac5a8e0209e5f7e6463c998e572006e
    REAL SHA-256 self_digest (turn 1)  = 017fe35c400a8072b564e0c84c2fd5e41fe12a916b6d938fa2ceead68b6b299b
[3] wrote b.txt with IDENTICAL content "hello world" -- blob_count before commit=1, after=1
[4] modified a.txt to "modified content" -- REAL new tree digest = ae0490d6...  (turn_index=3, differs
    from turn 1's e851a6df...)
[5] REAL Ledger.reset_to(target=1): new turn_index=4, restored tree digest=e851a6df... (matches
    turn 1's tree digest exactly: 1)
[6] AFTER real materialize(): a.txt on REAL disk = "hello world"; b.txt exists on real disk = 0
ALL CHECKS PASSED
```

Three real, load-bearing properties, each confirmed by an actual re-read of real state, not an
in-memory assertion:

- **Real content-addressed dedup**: `b.txt` written with byte-for-byte identical content to `a.txt`
  produces `blob_count() == 1` before AND after the commit — one real SHA-256-addressed blob backing
  two different tree entries, exactly 025 §2's dedup claim, now proven against the real store rather
  than assumed to hold because the design document says it should.
- **Real digest divergence**: modifying `a.txt`'s content produces a genuinely different SHA-256
  tree digest (`ae0490d6...` vs. `e851a6df...`), and rolling back reproduces the ORIGINAL digest
  exactly (`e851a6df...` again) — a real cryptographic fingerprint of state, not a coincidental
  string match.
- **Real rollback restores real bytes on real disk**: after `materialize()`, `a.txt` genuinely reads
  back `"hello world"` (not `"modified content"`) and `b.txt` genuinely does not exist on disk at
  all — the rollback did not just move a pointer in the `Ledger`, it rewrote the actual working
  directory's actual contents, verified by actually reading them back afterward.

Re-run three times for stability (identical output every time) and re-verified against the full
8-probe-directory regression suite (`identity_authority`, `async_quota`, `sandbox_session`, `ledger`,
`grant_set`, `integration`, `full_stack`, `worktree_io`) — every probe set green.

### 27.4 What this prove phase does and does not establish

**Established**: this design's `Ledger`/`SandboxSession` abstractions compose cleanly onto the REAL,
already-shipped, cryptographically-real `agentengine::WorktreeObjectStore`/`compute_digest` primitives
with no friction beyond the expected type substitution (§23's placeholder `Digest` swapped for the
real one) — real content-addressed dedup, real digest divergence/convergence, and a real rollback that
provably restores real bytes on real disk are all now demonstrated against actual SHA-256 hashes and
actual filesystem state, not any in-memory or string-based stand-in. The design's own line between
"reusable, capability-agnostic content-addressing" and "must stay on this design's own `GrantSet`,
never the old `Capability` system" held up under a real attempt to wire real I/O, without needing to
blur it.

**NOT established**: durability across a process restart (`InMemoryWorktreeObjectStore` is explicitly
non-durable, matching its own real file-header comment — a `File`-backed conformer is a real, tracked,
separate follow-up per that file's own words, not attempted here); real materialize/harvest under
concurrent multi-writer contention (this probe is single-threaded, proving CORRECTNESS of the
content-addressing and I/O round-trip, not re-proving concurrency already covered per-primitive in
§21/§22); symlink/path-traversal safety for real host paths (`RealIoFileSystem` trusts `relative_path`
directly — a real, disclosed gap this probe's own scope never intended to close, matching §22's own
disclosed native-shell mediation caveats); and a real merge algorithm, still explicitly out of scope
per §11.

## 28. Prove phase, part 9 — closing every remaining named gap: safety, durability, merge, concurrency

Source: additional files under `docs/planning/proofs/worktree_io/` and
`docs/planning/proofs/identity_authority/probe_concurrent_adopt.cpp`. This pass closes every item
§27.4 (and the still-open items from §25.4/§26.4) named as not yet established, in one sitting, per
explicit direction to finish them before moving on to a dedicated sandbox-safety review. **A second
real, previously-undiscovered concurrency bug was found and fixed in the course of this — a genuine
segfault, not a theoretical worry — recorded honestly below, not smoothed over.**

### 28.1 Path-traversal and symlink-escape rejection — a real, previously-missing check, now closed

`RealIoFileSystem` (§27) shipped with **no path-safety check at all** — a real, disclosed gap at the
time. Two independent, layered checks were added: `reject_unsafe_relative_path()` (lexical — rejects
any absolute path or a path containing a literal `..` component, the same real-project precedent
`sandbox::authorize_spec()`'s own comment already documents for exactly this reason) and
`reject_symlink_escape()` (resolves the full path through any existing symlinks via
`std::filesystem::weakly_canonical` and verifies the result still lives under the sandbox root — a
DIFFERENT attack the lexical check cannot see: a pre-existing symlink with no `..` anywhere in the
literal path string). Both proven against real attack attempts, not just unit-tested in isolation:

```
[1] write("../escape.txt"): REJECTED (real_io.path_traversal_rejected) -- nothing escaped
[2] write("a/b/../../../escape2.txt"): REJECTED
[3] write(<absolute path outside host_root>): REJECTED
[4] write("safe/nested/path.txt"): PASS (legitimate nested writes still work)
[5] write("link_to_outside/escape5.txt") through a REAL symlink to outside host_root_: REJECTED
    (real_io.symlink_escape_rejected), file actually escaped to real disk outside sandbox=0
```

Check [5] created a genuine `std::filesystem::create_directory_symlink` pointing outside the sandbox
root and attempted to write through it — this environment permitted real symlink creation, so the
check ran for real rather than being skipped, and confirmed the write was rejected with nothing
landing outside the sandbox. **Honestly disclosed, not closed**: this remains a check-then-use
(TOCTOU) gap — a symlink created *after* this check returns but *before* the actual `ofstream` open
races it, the identical class of residual §11/§22 already disclose for native-shell mediation rather
than claim solved.

### 28.2 Durability across a simulated process restart — a real, file-backed `WorktreeObjectStore`

A new `FileWorktreeObjectStore` (one file per blob/tree, named by its own real digest — conforms to
the exact same `agentengine::WorktreeObjectStore` concept `InMemoryWorktreeObjectStore` does, a
drop-in alternative) was built and proven durable: a first store INSTANCE writes real blobs/trees to
real disk and is destroyed; a SECOND, completely independent instance, constructed later and sharing
no in-process state with the first, reads the identical content back from the same real directory —
and a THIRD instance confirms content-addressed dedup survives the "restart" too (re-putting identical
content produces the same digest and does not grow `blob_count()`). All pass, first run.

### 28.3 A real three-way merge — and a real logic bug this probe's own first run caught

`merge_trees(base, ours, theirs)` was implemented for real (standard three-way semantics: a path only
one side touched takes that side; a path both sides converge on identically is not a conflict; a path
both sides change to genuinely different values is a real, reported conflict, never silently
resolved). **The first version had a real bug**, caught on the probe's first actual run, not by
inspection: a path that is NEW on exactly one side (absent from `base` entirely) was incorrectly
routed into the conflict branch, because the original "unchanged relative to base" check only compared
digests when a base entry existed at all, never handling "absent from base AND absent from the other
side" as its own form of agreement. Fixed by redefining "side X unchanged relative to base" to account
for presence, not only digest equality when present. Re-run, 6 cases now pass: clean non-overlapping
additions, only-one-side-modified, both-sides-converge-identically, a genuine conflict (with correct
base/ours/theirs digests reported), delete-vs-modify (also correctly flagged as a real conflict), and
a combined, realistic 4-file/1-conflict scenario.

### 28.4 A SECOND real segfault found and fixed — `Ledger`'s shared object store was never actually thread-safe

Stress-testing concurrent multi-session commits against one shared `Ledger` **segfaulted on the first
real run** — not a hang, not a wrong answer, a genuine crash. Root cause, found by inspection of the
real, already-verified `agentengine::InMemoryWorktreeObjectStore` (re-read for this pass): it has **no
internal synchronization of its own** — its real `std::unordered_map` members are plain and unguarded.
`Ledger::commit()`/`create_root_branch()` called `store_.put_tree()` **before** acquiring `mutex_`, so
two concurrent commits raced two unsynchronized map mutations on the identical store instance — a
textbook concurrent-`unordered_map`-corruption crash. A second, subtler instance of the SAME root
cause was found by inspection before it could be independently rediscovered: `Ledger::object_store()`
handed out a raw, mutable reference for EXTERNAL callers (this design's own `RealIoFileSystem::
drain_into_tree()`/`materialize()`) to mutate directly, completely bypassing `Ledger`'s own `mutex_`
regardless of how carefully `Ledger`'s own internal methods were fixed.

**Fixed by removing the raw accessor entirely**: `Ledger` now exposes only `put_blob_safe()`/
`get_blob_safe()`/`get_tree_safe()`/`blob_count_safe()` — every one of them, and every internal
`commit()`/`create_root_branch()` call, goes through the exact same `mutex_`, with no path left that
touches the real object store outside it. `RealIoFileSystem` was updated to take `Ledger&` instead of
a raw store reference, using only the safe accessors. Re-verified: the original concurrent-session
probe (12 sessions × 100 commits, no I/O) now passes consistently across 5 runs; a NEW probe exercising
the exact scenario that would have hit the SECOND half of this bug (10 independent sessions, each with
its own real host directory, all draining real I/O into the SAME shared `Ledger` concurrently — 500
real writes/drains/commits total) also passes consistently across 5 runs, with `blob_count() == 500`
exactly (every write's content was unique, so no accidental dedup masking a lost commit).

This is the second genuine concurrency-crash-class bug this prove phase has found through actual
multi-threaded execution rather than static reasoning (the first being §21.1's `AsyncMutex`-by-value
non-movability finding) — both were real, both were found by running code under real contention, not
by any amount of design-document reasoning, which is exactly the empirical case this project's own
`design → red-team → prove → judge` discipline makes for insisting on a real `prove` stage rather than
treating exhaustive textual red-team as sufficient on its own.

### 28.5 Concurrent `IdentityAuthority::adopt()` under real contention

Three real multi-threaded scenarios, all passing consistently across repeated runs: (1) 16 threads ×
500 calls racing to adopt the SAME real id concurrently — all 8,000 results resolve to one identical
internal id; (2) 16 threads × 200 calls each adopting DISTINCT real ids concurrently — 3,200 total
adoptions, 3,200 unique internal ids, zero collisions; (3) 50 distinct real ids, each raced by 8
threads at the exact moment of its OWN first-ever adoption (the specific race a warm-cache re-adoption
test like (1) cannot exercise) — every one resolves consistently. `adopt()`'s plain `std::mutex`
choice (§15.1/§16's own "identity minting is rare, a plain mutex is the right tool" reasoning) holds
under real load, including the specific first-registration race its own design reasoning never
explicitly walked through.

### 28.6 What this prove phase does and does not establish

**Established**: every item named open across §25.4/§26.4/§27.4 is now closed by real, passing,
repeated-run code — path-traversal and (same-environment) symlink-escape rejection, durability across
a simulated restart, a real three-way merge with a real bug found and fixed on first run, real
concurrent multi-session `Ledger` load (after fixing a real segfault), real concurrent multi-session
I/O against a shared `Ledger` (after extending that same fix to close the external-accessor half of
the bug), and real concurrent `IdentityAuthority::adopt()` contention. Two independent, real
concurrency-crash bugs were found by this prove phase's own execution across its full history
(`AsyncMutex`-by-value non-movability, §21.1; the unsynchronized shared object store, this section) —
both fixed, both disclosed with the exact mechanism that caused them, not merely patched silently.

**Still not established, named plainly rather than implied closed**: the TOCTOU symlink-race residual
named in §28.1; whether `FileWorktreeObjectStore`'s one-file-per-object layout scales to a real
deployment's object count (a flat directory, not git's own fan-out scheme — untested at volume);
`merge_trees()`'s own conflict-RESOLUTION policy (this function detects and reports conflicts
correctly; deciding what a caller should DO with a reported conflict — block, prefer a side, ask a
human — is a product decision this prove phase does not make); and the full `AgentSession`/
`ContextProvider`/`Tool<>` integration story for real Ledger/SandboxSession wiring at production scale
(§26's full-stack probe remains the single most complete integration evidence, but it is still one
session, one thread, one turn sequence — not a production-scale harness). Per the user's own explicit
direction, the next phase is a dedicated review of this design's sandbox SAFETY properties
specifically — building on, not repeating, the concurrency and path-safety work this section already
closed.

## 29. Internal attack simulation — real, adversarial attacks against the real, running stack

Source: `docs/planning/proofs/attack_sim/probe_internal_attacks.cpp`. Per explicit direction: assume
an attacker already runs INSIDE the sandbox process (a malicious or compromised tool body, sharing the
same process as everything else — matching this design's own established threat model, §16/§19.1:
native in-process code is trusted BY THE ENGINE'S OWN DESIGN, but "trusted" is a design decision about
where responsibility sits, not a claim that malicious native code is harmless). Five real attacks were
attempted against the real, already-proven stack — not a toy model, not a paper exercise. **Two
produced confirmed, real, previously-undisclosed leaks, fixed in the same pass; one confirmed an
already-disclosed gap for real; two confirmed the design's own defenses genuinely hold under a hostile
attempt, not just cooperative testing.**

### 29.1 Attack 1 (CONFIRMED LEAK, then fixed) — any caller could read any session's content by digest

`Ledger::get_blob_safe()`/`get_tree_safe()` had **zero identity check** — any code holding a `Ledger&`
reference (the real, tested multi-tenant shape from §26/§28, where multiple sessions legitimately
share one `Ledger`) and a digest could read content committed by ANY OTHER session, with no
relationship to it whatsoever. Confirmed for real: `attacker_owner` (a sibling principal, zero
ancestry relation to `victim_owner`) called `get_blob_safe(secret_blob_digest, attacker_owner)` and
successfully read victim's real secret content — `"TOP SECRET: victim's private data"`, read back
verbatim.

**Fixed**: every blob/tree digest now carries a real ACL (`std::set<std::uint64_t>` of authorized root
principal ids), populated at the moment a principal legitimately writes that digest
(`put_blob_safe`/`commit`), checked on every read via the same real, already-proven multi-hop
`IdentityAuthority::is_ancestor_of()` table §20/§25 built and verified. Re-run after the fix:
`get_blob_safe(digest, attacker_owner)` correctly returns `ledger.blob_access_denied`; a sanity check
in the SAME run confirms `victim_owner` can still read its own data (the fix does not also break the
legitimate case — checked explicitly, not assumed).

**Why this matters beyond the immediate fix**: the practical exposure was never "brute-force a
SHA-256 digest" (infeasible) — it was that this design's OWN tooling routinely **surfaces** digests in
places an attacker sharing the process could observe them: §26/§27's own `reset_sandbox` JSON reply
prints the real tree digest directly into a model-facing tool response; a shared `Ledger`'s checkpoint
history is itself a real, queryable structure. Treating a 64-character hex string as a secret was never
a sound access-control model on its own — the sharing decision (one `Ledger` across sessions, made
explicitly for real content-addressed dedup benefits in §21/§27/§28) needed a REAL authorization layer
to be safe, not merely an assumption that digests wouldn't leak.

### 29.2 Attack 2 (CONFIRMED LEAK, then fixed) — committing a tree could "adopt" another session's content

`Ledger::commit()` accepted a tree from `attacker_owner` whose entries referenced
`secret_blob_digest` — content `attacker_owner` never wrote, had no grant for, and had no
relationship to — durably attributing victim's secret content into attacker's own committed
checkpoint history. Confirmed for real, then re-read successfully through attacker's own tree before
the fix.

**Fixed by the same ACL mechanism**: `commit()` now validates, BEFORE accepting the tree at all, that
`authored_by` (or an ancestor) is authorized for every entry's digest already in the store — a
`TreeEntry` referencing an unauthorized digest fails the WHOLE commit closed
(`ledger.commit_unauthorized_reference`), not merely the reference. Re-run after the fix: the commit
is correctly rejected before any state changes. This is the same root fix as Attack 1 — one ACL
mechanism closes both the direct-read and the reference-at-commit-time variants of the identical
underlying gap (a shared content store with no per-digest authorization).

### 29.3 Attack 3 (blocked, confirmed holding under real hostility) — sibling grant misuse

`attacker_owner` (a genuine sibling of `victim_owner`, not a descendant) attempted to use a
`Grant<RollbackAuthority>` issued only to `victim_owner`, via `GrantSet::find()`. Correctly refused —
`IdentityAuthority`'s real, multi-hop ancestry table has no path connecting two independently-minted
root principals, and `GrantSet`'s lookup genuinely depends on that table rather than any
shape-matchable credential. This is the real, structural closure of the historical (machinery-reusing)
design's own confirmed cross-principal griefing finding — proven here under a genuinely adversarial
attempt (an unrelated sibling, not a cooperative descendant), not only the cooperative test cases
§20/§25 already ran.

### 29.4 Attack 4 (structurally blocked) — cross-principal branch abandonment, the historical design's own named vulnerability

The historical (machinery-reusing) design's own round-4 red-team confirmed a real, exploitable gap:
`CapabilitySet` has no identity field, so anyone holding an equivalently-SHAPED grant could abandon a
different principal's branch. This design's structurally different gate — `abandon()`/`merge()`
require literally OWNING a `BranchHandle` OBJECT (private constructor, friend `Ledger` only, handed
back only to whoever just called `create_root_branch()`/`branch_from()`) — has **no shape to forge,
only an object to not have**. Verified by exhaustively reviewing `Ledger`'s real public surface: no
method exists anywhere that hands out a `BranchHandle` for an existing branch by name. This is a real,
confirmed architectural improvement over the historical design's own finding, not merely a differently
-shaped version of the same hole.

**Honestly disclosed residual, not claimed closed**: this closes the DIRECT attack only. A
confused-deputy variant remains open and untestable until a real integration layer exists: if HOST
code itself is manipulated (e.g. by a malicious tool's return value influencing which branch name a
LATER, legitimate, host-driven `abandon()` call targets), object-possession alone does not protect
against the host being the one confused — an I3-shaped concern for whatever future integration layer
actually drives real `abandon()` calls from tool/model-adjacent signals.

### 29.5 Attack 5 (confirmed real, already disclosed in principle) — native code bypassing mediation entirely

A simulated malicious tool body wrote directly to `victim`'s real sandbox directory via a bare
`std::ofstream`, completely bypassing `RealIoFileSystem`/`MediatedFileSystem`, every path-safety
check, every ACL, and every attribution mechanism this design built. It succeeded trivially — the
file exists, unmediated, unattributed. This confirms, with a real write rather than a hypothetical
one, the exact I4 gap the main design document's own §9 already disclosed: this design's entire
`GrantSet`/path-safety/digest-ACL model governs only code that CHOOSES to go through
`MediatedFileSystem` — it is not, and cannot be, an OS-level enforcement boundary against a native
tool's own arbitrary code running in the same process. Closing this for real would require an actual
OS-level jail (the native_jail backend's own real, existing job) around the execution surface itself,
not another layer inside this design's own primitives — named accurately here as boundary-of-scope,
not smoothed over as solved.

### 29.6 What this attack simulation does and does not establish

**Established**: two real, previously-undisclosed, confirmed data-isolation leaks in the shared
`Ledger` content store (cross-session blob read, cross-session tree-reference "adoption") — found by
genuine adversarial attempt, not code review, and fixed with a real, re-verified identity-scoped ACL
mechanism reusing the same ancestry infrastructure already proven sound; the historical design's own
confirmed cross-principal branch-abandonment vulnerability is structurally closed in this design, not
merely relocated, verified under a genuinely adversarial (sibling, not cooperative) attempt; and the
already-disclosed native-code-bypass boundary is now demonstrated with a real file write, not only
asserted in prose.

**NOT established**: the ACL fix's own behavior under real concurrent read/write contention (added
after §28.4's concurrency work, not yet re-stress-tested the same way); whether per-digest ACL sets
need their own storage-growth bound (an unbounded `set<uint64_t>` per digest, in a long-running
deployment with many sessions ever touching shared content, is a real, unexamined growth vector,
symmetric to §8's original storage-growth concern but never named until now); the confused-deputy
variant of Attack 4 (explicitly deferred, no integration layer exists yet to test it against); and
whether the OS-level jail Attack 5's real fix would require is itself sound (out of this design's own
scope — that is `native_jail`'s existing, separately-proven job, not something this identity-native
design redesigns).

**UPDATED by §32**: at the time this section was written, this attack simulation's own hardened
Ledger had never been composed into a real `SandboxSession` — meaning "the composed stack resists
these attacks" was never actually demonstrated, only "this standalone Ledger resists these attacks
when driven directly." §32 closes that gap for real, composing this exact Ledger into `full_stack/`
and adding a capstone check proving the composed artifact itself rejects the Attack 1/2 shape.

## 30. What actually happens today when Shell calls Python and Python touches disk (real, shipped behavior, not this design)

Asked directly, answered by reading the real, current, shipped source — not this document's own
proposed primitives, which have never been wired into either real path. Full citations in the research
transcript this section summarizes; the mechanism, restated precisely:

**Guest Python `open(path, mode)` → real host file, step by step**: `builtins.open`/`io.open` are
monkey-patched (`python_worker_mediation.cpp:749-752`) to call `_ae_internal.open`
(`Internal_open`, C) → `split_guest_path()` splits the guest path into `(mount_id, mount_relative)` →
a JSON query crosses the ONE real IPC bridge out of the worker process (`query_or_raise`/
`g_query_fn`) → the HOST process's `dispatch_open` (`native_jail_backend.cpp:940-1031`) looks
`mount_id` up in that worker's own `mount_roots` map, performs a REAL capability check
(`ctx.capabilities->find_fs_write`/`find_fs_read` — the existing, real `CapabilitySet`, not this
design's `GrantSet`) plus a live on-disk quota check, then does a genuine Win32 `CreateFile` under
whatever real host directory `mount_roots[mount_id]` names. The guest process never holds a real OS
file handle at all — every subsequent `.read()`/`.write()`/`.close()` is a further IPC round trip
relayed through an opaque `file_id` (`_AeRelayFile`), with the real handle staying in the host
process the whole time.

**Shell invoking Python as a composed runner (`python script.py` typed at the shell)**: dispatched via
`mediated_command_registry.hpp`'s `register_runner()`/`cap::RunnerCall` mechanism
(`mediated_shell_dispatch.cpp:243-280`). Gated by a real capability check
(`cap::RunnerCall{"python"}` must be explicitly granted, checked via `ctx.capabilities->contains(...)`
— without it, Python is never reached at all, confirmed by a real test). If granted, the invoked
Python runner receives the **identical** `EffectContext&`/`CapabilitySet` and the **identical**
`ExecState&` (cwd/env) the shell itself was using — Python cannot exceed shell's own capability
ceiling. **But the filesystem mount is NOT shared**: `MediatedPythonRunner`'s own `mount_roots` map is
fixed once, independently, at that runner's own construction time
(`MediatedPythonConfig::mount_roots`) — entirely disconnected from whatever `FileSystemAdapter`/mount
the calling `MediatedShellRunner` uses. In the real, current production wiring
(`tools/cli_chat.cpp`'s `shared_python_runner()`), Python's mount points at its own separate
`agentengine_cli_chat_workspace` scratch directory; Shell's `SessionShellSandbox` points at whatever
directory its own caller supplied — two different real directories, confirmed by a real composition
test (`test_mediated_shell_runner_python_composition.cpp`) where Python's config has NO `"work"` mount
entry at all, so opening `/work/...` from Python fails on the mount lookup itself, before capability
is even consulted.

**If Python code requests disk (the literal question asked)**: the request is genuinely
capability-checked (cannot exceed what the shell that invoked it holds) and quota-checked against
live usage, and — if authorized — real bytes land via `WriteFile`/`ReadFile` on a real host directory.
**But nothing durably records the write as a Tree/Ref/commit.** `worktree_mount_sync.hpp`'s
`materialize_mount()`/`harvest_mount()` — the real, tested machinery that COULD bridge a mount into
the worktree Tree/Ref system — has **zero production callers**; `dispatch_open`/`dispatch_file_write`
never call it. Multiple existing planning documents state this outcome explicitly, not merely as an
inference: `mandatory-session-worktree-design.md:389` — *"Python's writes are invisible to this
design's worktree entirely, today."*; `ADR-096:290-292` — *"Python's session-lifecycle wiring is
untouched... stays exactly as it is."* The practical answer: **a real, correctly capability-gated
write happens, on real disk, and then simply exists — unattributed to any Tree/Ref, uncaptured by any
checkpoint, invisible to any rollback** — this document's own `Sandbox`/`Ledger`/`MediatedFileSystem`
primitives (§20-§29) are a PROPOSAL that has never been wired to either the real Shell OR the real
Python path; both remain exactly as disconnected from each other, and from any worktree, as the
historical design's own §9 already disclosed.

## 31. A real Docker container as an execution surface — real OS-level containment, proven and integrated

Source: `docs/planning/proofs/docker_sandbox/`. Prompted by §29 Attack 5's own conclusion ("closing
this for real would require an actual OS-level jail... not something this identity-native design
redesigns") — this environment has a real, running Docker Desktop daemon (confirmed:
`docker version`/`docker ps` against genuinely running containers), so the question was tested
directly rather than left as an abstract "someday" note.

### 31.1 A real environment constraint found immediately, worked around, not hidden

`docker run -v <host_path>:/workspace` failed for every real path tried — the project's own drive,
`C:\Users\<user>`, the system temp directory — with `"the path ... is not shared from the host; add it
in Settings > Resources > File Sharing"`. This Docker Desktop installation's bind-mount allowlist
(a GUI-configured setting, confirmed via `docker context ls`/`wsl -l -v` to be the WSL2 backend) does
not include any path this probe could reach from the CLI, and changing it is a host environment
decision outside this session's own scope to make unilaterally. Worked around with `docker cp`
(copies real bytes across the container boundary without needing the bind-mount allowlist at all) —
disclosed as a real environment limitation this probe adapted to, not smoothed over as if a live bind
mount had been used.

### 31.2 Real containment, confirmed against a genuinely unreachable host secret

A real secret file was placed on real host disk, **never copied into the container by any means**. A
real container (no bind mount, no shared filesystem view of any kind) was asked, from the inside, to
find it by filename search and by direct path read — both failed (`No such file or directory`), while
the container's own real, functional filesystem (`/workspace`, created by its own `mkdir`) remained
fully usable. This is the real, structural property §29 Attack 5 named as missing from the
identity-native primitives alone: **a kernel-enforced mount namespace boundary that does not depend on
the contained code choosing to respect it** — categorically different from every check this design's
own `GrantSet`/`MediatedFileSystem` primitives perform, which are only ever consulted by code that
calls them.

### 31.3 Real bidirectional integration with the already-proven worktree stack

A file written **inside the real container** (`echo ... > /workspace/from_container.txt`) was copied
out via real `docker cp` onto the same real host directory this design's `RealIoFileSystem` already
reads. Draining it required a NEW method, `scan_and_drain_into_tree()` — the existing
`drain_into_tree()` only trusts a `touched_` set populated by this object's OWN `write()` calls, and is
blind to anything (a `docker cp`, or a bypassing native tool — the identical §29 Attack 5 shape) that
lands in the directory some other way. `scan_and_drain_into_tree()` does a REAL, full recursive
directory scan instead, capturing everything actually present regardless of how it arrived — the
honest fix DIRECTION for Attack 5's I4 gap (every byte gets captured and attributed to the committing
turn's owner; WHICH specific process wrote it is still not recoverable, the same disclosed limitation
§17.4/§29 already name for any bypassing write). Committed through the real, already-proven `Ledger`
(`turn_index=1`, a real SHA-256 tree digest) exactly as any other session's write would be. The reverse
direction was proven too: a file materialized from the host into a **different, freshly-created**
container via `docker cp`, content verified byte-for-byte with a real `cat` inside that new container.

### 31.4 A real, disclosed probe-hygiene defect found along the way

An earlier run of this probe hit a real `CHECK` failure mid-test (before `scan_and_drain_into_tree()`
existed) and called `std::abort()` — which, since the running Docker container was tracked only by a
plain `Instance` value with no RAII destructor, left a REAL container running after the probe process
died. Found and cleaned up manually (`docker rm -f`), and named here rather than silently fixed: this
probe's own `DockerBackend::Instance` has no destructor-driven cleanup — a real, disclosed gap in the
probe's own hygiene (not this design's production primitives), worth carrying forward as a lesson if
any of this ever becomes real integration code — a real backend wrapper should tie container lifetime
to an RAII handle, not to reaching the last line of a test function.

### 31.5 What this pass does and does not establish

**Established**: a real Docker container provides a genuine, kernel-enforced isolation boundary this
design's own in-process primitives cannot provide by construction, confirmed against a real,
unreachable secret; that boundary composes with the already-proven identity-native worktree stack
(§21-§29) through a real, working bridge (here: `docker cp`; a live bind mount would work identically
in an environment whose Docker Desktop allowlist permits it); and the exact same `scan_and_drain_into_tree()`
mechanism this integration needed is also a real, viable fix DIRECTION for §29 Attack 5's own
bypass-write gap, discovered as a side effect of solving an unrelated integration problem, not sought
out deliberately.

**NOT established**: resource-limit enforcement (`--memory`/`--cpus`/`--pids-limit`) — named in the
probe's own output as unexercised, not claimed; network isolation; a live bind-mount round trip (this
environment's own Docker Desktop configuration prevented testing it here); how a real `DockerBackend`
would integrate with `SandboxBackendRegistry`/the real `SandboxBackend` concept `KataBackend` conforms
to (this probe deliberately shells out ad hoc, matching `KataBackend`'s own real `ctr`-CLI precedent,
but was not built AS a conforming `SandboxBackend` — a real, disclosed scope narrowing, not an
oversight); and whether `scan_and_drain_into_tree()`'s full-directory-scan approach is performant
enough for a real, large working tree (this probe's own directories are trivially small).

## 32. Meta-review — a code review of the prove phase itself, and the real fixes it forced

Prompted by an explicit instruction to stop expanding scope and instead review the prove-phase work
already done: an independent adversarial code-review pass (a fresh agent, no memory of this
document's own narration of its own probes) was run against every file under
`docs/planning/proofs/` except `docker_sandbox/` (§31, already independently reviewed on its own
terms) and the production `tests/` additions (out of this design's own scope). Mandate: cross-file
consistency between what a `§NN` section *claims* and what the code *actually does*; whether the
three recurring bug classes this document already tracks (non-movable-sync-primitive-by-value,
`operator[]` needing default-constructibility, `co_await` misuse) recur anywhere new; general
adversarial review for contradictions or regressions.

### 32.1 The central finding — two Ledgers, not one, and no artifact proving the composed stack is safe

The review's headline finding, confirmed by direct inspection, not asserted: `full_stack/
real_sandbox_session.hpp` (§26's "full stack" demo) included `../ledger/ledger.hpp` — §23's ORIGINAL
Ledger (FNV-1a placeholder digests, no blob storage, no ACL, but real `branch_from()`/`merge()`).
`attack_sim/probe_internal_attacks.cpp` (§29's attack simulation) included `../worktree_io/
worktree_ledger.hpp` — a DIFFERENT, later-built Ledger (real SHA-256 content-addressing via the
actual `agentengine::WorktreeObjectStore`, the real §28.4 concurrency fix, the real §29 ACL fix), but
which the review additionally discovered (grep-confirmed, zero hits) had **no `branch_from()`/
`merge()` at all** — a feature-surface regression nobody had named until this pass.

The consequence, stated plainly rather than smoothed over: **no single Ledger implementation
anywhere in this tree ever had real content-addressing + the concurrency fix + the ACL fix +
branch/merge all at once**, and therefore **§26's "full stack" demo and §29's "attacks confirmed and
fixed" demo were claims about two different, non-overlapping artifacts** — the full-stack demo was
never actually vulnerable to (or protected against) the §29 attacks, because its own Ledger had no
shared blob store for those attacks to target in the first place. Every "the full stack is sound"
sentence in §26.4 rested on a Ledger that had never been attacked, and every "the attacks are fixed"
sentence in §29.6 rested on a Ledger that had never been composed into a session.

### 32.2 The fix — real unification, not a documentation patch

Given this project's own standing discipline (prove with a compiler and a real run, not more prose),
this was fixed in code, then re-verified, not merely disclosed:

1. **`branch_from()`/`merge()` added to `worktree_io/worktree_ledger.hpp`** — real COW branching
   (the child starts pointing at the parent's current head tree/digest, no content copy needed since
   content is digest-addressed; the child's creator is granted ACL read access to the inherited head
   tree) and a real, stub-level merge (matching the original's own already-disclosed "no true 3-way
   content merge" scope boundary, §11) that validates the child's tree ACL before letting the parent
   adopt it — a merge cannot smuggle unauthorized content into the parent's history either.
2. **`full_stack/real_sandbox_session.hpp` and `real_reflector.hpp` rewired onto
   `worktree_io/worktree_ledger.hpp`** — the SAME Ledger §29 hardened, not a parallel copy.
   `combine_into_tree()` (previously a hand-rolled string the code claimed was "sorted" but never
   actually sorted — a real comment/code mismatch the review also caught) now puts each staged write
   through `Ledger::put_blob_safe()` and builds a real `agentengine::Tree`, which
   `agentengine::WorktreeObjectStore::put_tree()` genuinely sorts before hashing — the sortedness
   claim is now actually true, enforced by the real store, not by a promise in a comment.
3. **A capstone check added to `probe_full_stack.cpp`**, the thing the review explicitly said was
   missing: after driving two real turns and a real rollback through the composed `SandboxSession`,
   an unrelated outsider principal (no ancestry relationship to the session's owner) attempts
   `session.ledger().get_tree_safe()` against the session's own just-restored tree digest, over the
   SAME shared Ledger — and is rejected, with a sanity check confirming the legitimate owner's own
   read of the identical digest still succeeds. Real output, this run:

   ```
   [5] CAPSTONE: an unrelated outsider principal (no ancestry relationship to bridged_owner)
   attempting to read THIS composed SandboxSession's own restored tree digest via the SAME shared
   Ledger: REJECTED (caller is not authorized to read this tree digest) -- the composed full stack
   genuinely resists the §29 Attack 1/2 cross-session read, in place, not merely in a separate
   standalone probe.
       sanity check: the legitimate owner's own read of the same digest still succeeds (no
   regression from the ACL check): PASS
   ```

   This is the piece that was actually missing before: not "does a Ledger resist this attack" (§29
   already showed that) but "does the composed, driven-through-real-turns artifact this document
   calls 'the full stack' resist it" — now demonstrated, not merely inferred by proximity of two
   separately-true facts.

### 32.3 A second, independent divergence the rewire itself exposed

Making the swap surface a SIXTH instance of a bug class this document had already named twice:
`worktree_ledger.hpp`'s `BranchHandle` had `operator=(BranchHandle&&) = delete;`, while the ORIGINAL
`ledger/ledger.hpp`'s `BranchHandle` implements a real move-assignment operator (overwriting a live
handle first queues-abandon of whatever it previously held). This was invisible until
`SandboxSession`'s own defaulted move-assignment — silently, only-a-warning-not-an-error — became
implicitly deleted the moment `SandboxSession` started holding the rewired `BranchHandle`. Fixed by
implementing the real move-assignment identically to the original's own version, closing yet another
previously-undisclosed API-surface gap between the two Ledgers that the divergence itself had hidden
from view.

### 32.4 Smaller findings, each fixed and re-verified for real, not only documented

- **`worktree_ledger.hpp::reap_pending_abandons()` had REGRESSED a bug class §23.1 already fixed
  elsewhere**: it unconditionally called `IdentityAuthority::bootstrap().mint_root("reap-placeholder")`
  on every loop iteration, minting a real, immediately-discarded `Principal` even when the branch
  turned out not to exist — the exact "speculative mint before confirming it's needed" shape §23.1
  named and fixed in the original Ledger. Fixed via `std::optional<Principal>`, minting nothing
  unless a real creator is actually found.
- **`sandbox_session/media_fs.hpp` still had the by-value `std::mutex sync_mutex_` bug** (§21.1's
  pattern) that a separately-written copy of the same class, embedded in
  `full_stack/real_sandbox_session.hpp`, had already independently found and fixed via
  `unique_ptr<std::mutex>` — this original, standalone copy was never patched to match, harmless only
  because its own probes (`probe_two_lock_safe.cpp`, `probe_deadlock_demo.cpp`) never move it by
  value. Fixed identically.
- **`grant_set/grant_set.hpp` had NO synchronization at all** — unlike `IdentityAuthority`,
  `AsyncQuota`, `Ledger`, and `MediatedFileSystem`, all of which received an explicit concurrency
  pass, `GrantSet` never did, despite this document's own §18 already discussing
  `tool_pipeline.hpp::background_task()` dispatching real work on a detached `std::thread` — a real,
  plausible concurrent-access scenario against a session-owned `GrantSet` that was simply never
  named. Fixed via a `unique_ptr<std::mutex>` (not by-value, for the now-familiar §21.1 reason —
  `GrantSet` is moved by value elsewhere, e.g. `SandboxStandIn`'s constructor).
- **`worktree_io/file_object_store.hpp`'s `get_blob()`/`get_tree()` built filesystem paths directly
  from a caller-supplied digest string with zero validation** — not exploitable through the one real
  gated caller in this tree today (`Ledger` only ever passes digests it computed itself), but a real
  latent path-traversal surface given this file's own header comment frames it as "a drop-in
  replacement `Ledger` could hold instead." Fixed with a real 64-char-lowercase-hex well-formedness
  check before any path is built, and proven — not just asserted — with a new case in
  `probe_durability.cpp` that plants a real secret file outside the store's root, attempts to reach
  it via a crafted `"../..."` digest string, and confirms the attempt is rejected before the
  filesystem is ever touched. It also had the same by-value `std::mutex` gap as `media_fs.hpp` above;
  fixed the same way, same reason.

### 32.5 Full re-verification — every touched probe recompiled and re-run for real

Per this project's own standing rule (a real compiler and a real run, not a description of one),
every probe that includes any file this pass touched was recompiled (`clang 22.1.5`, target
`x86_64-pc-windows-msvc`, `-std=c++23`, linking the real `src/core/worktree_digest.cpp` where a real
digest is needed) and re-run to completion after the fixes above, not just the new/changed ones:
`worktree_io/probe_concurrent_io`, `probe_concurrent_ledger`, `probe_durability` (extended with the
new traversal case), `probe_merge`, `probe_path_traversal`, `probe_worktree_io`,
`attack_sim/probe_internal_attacks`, `grant_set/probe_positive`, `integration/probe_integration`,
`full_stack/probe_full_stack` (extended with the capstone case) — **ALL CHECKS PASSED** on every one,
with `docker_sandbox/probe_docker_sandbox.cpp` (out of this review's own scope, but a real dependent
of `worktree_ledger.hpp`) separately confirmed to still compile clean against the changed Ledger.

### 32.6 What this meta-review pass does and does not establish

**Established**: this document's own two most complete integration artifacts (§26's "full stack" and
§29's "attacks confirmed and fixed") are now genuinely the SAME artifact, not two artifacts that
happened to share a document section number; the composed, driven-through-real-turns `SandboxSession`
demonstrably rejects the exact cross-session read attack §29 found, proven in place rather than
inferred from two separately-true facts; five additional real, previously-undisclosed gaps (a
feature-surface regression, a bug-class regression, a stale unfixed copy of an already-fixed bug, a
missing concurrency pass, and an unvalidated-path latent surface) are closed with real code and a
real re-run, not merely named; and the project's own "prove, don't assert" discipline caught a real
regression (`reap_pending_abandons()`) that a purely textual re-read of the code would very plausibly
have missed, since the code superficially resembles the already-fixed original.

**NOT established**: whether reviewing prove-phase code for cross-file consistency this way should
become a standing step after every future prove-phase expansion in this document (this was a
one-time, explicitly requested pass, not a proposal to institutionalize it); whether the newly-added
`branch_from()`/`merge()` on the unified Ledger has itself been attacked the way §29 attacked
`commit()`/`get_blob_safe()`/`get_tree_safe()` (it has not — this is new surface area introduced BY
this fix, and inherits no automatic coverage from §29's own, earlier attack simulation); and whether
any further undiscovered divergence exists between the (now fewer, but not necessarily zero) files in
`docs/planning/proofs/` that were never each other's stated dependency — this review was thorough,
not exhaustive, and this document's own history (this section included) is repeated, direct evidence
that composing independently-proven pieces has surfaced a new seam every time it has been tried.

## 33. Red-team round 5 (post-prove-phase) — identity durability vs. durable-ACL compatibility

A dedicated, independent red-team pass, deliberately scoped away from re-checking anything §1-32
already named: pick ONE architecture-level property this document has not yet examined, and try hard
to break it for real, not in prose. Run by a fresh agent with no memory of this document's own prior
narration of its own findings (avoiding the bias of an agent that already "knows" what's been fixed),
then independently reproduced a second time from scratch before being trusted enough to write down
here — this project's own standing rule (a real compiler and a real run, not a description of one)
applied to the red-team step itself, not just the prove step.

### 33.1 The gap: two different things this design makes durable, on two different schedules

`docs/planning/proofs/worktree_io/file_object_store.hpp` (§28.2) gives the content-addressed store
real, crash-safe durability — a real file survives a real process exit. But
`docs/planning/proofs/identity_authority/identity_authority.hpp`'s `IdentityAuthority` is a Meyer's
singleton with a **plain, in-memory, per-process sequential counter** (`next_id_`, starting at 1 on
every fresh process) and no persistence of its own, anywhere. `worktree_ledger.hpp`'s real ACL fix —
the exact fix §29 added to close a confirmed cross-session leak — keys `blob_acl_`/`tree_acl_` by
that same raw, recyclable `std::uint64_t`. §11's own first open question ("what actually backs
`Ledger`'s object/blob storage for a real deployment... a durable, crash-safe, content-addressed
store") is asking someone to build exactly the kind of persistence that turns this into a real
vulnerability: the moment ACL state is made durable across a restart while identity is not, a
brand-new, completely unrelated principal minted after the restart can be assigned the exact same
numeric id a prior principal held — and inherit its durable authorizations by pure accident of
counter timing, not by any exploit.

### 33.2 Real, reproduced twice, in genuinely separate OS processes

Two standalone probes (investigation-only, outside `docs/planning/proofs/`, no repo files touched)
reuse the real `IdentityAuthority`, the real `FileWorktreeObjectStore`, and an exact copy of
`worktree_ledger.hpp`'s own `authorized_for()` logic against a plausible persisted ACL record (digest
→ owning principal id — precisely the shape closing §11's own durability question would produce).
`write_side.exe` mints "Alice," writes a real durable blob, and persists an ACL record naming Alice's
id as the authorized owner, then exits (its process, and with it `IdentityAuthority`'s entire
in-memory state, is gone). `read_side.exe` — a genuinely separate, later-launched OS process, not an
in-process "pretend restart" — mints an entirely unrelated principal, "Mallory," first thing, before
ever touching the ACL file.

Compiled clean (`clang 22.1.5`, `-std=c++23`, real headers, real `agentengine::compute_digest` via
`src/core/worktree_digest.cpp`/BCrypt) and run as two separate process invocations, independently
reproduced by this document's own author a second time (not just taken on the red-team agent's word):

```
=== RUN 1 (write_side, separate process) ===
[write_side] minted principal 'session-for-alice' -> id=1
[write_side] wrote real durable blob (digest=fb54a9abf66287ada914c5c8dc50eb3d3836c0a4423e5d2c457d4368ed355777) owned by principal id=1
[write_side] (this process now exits -- IdentityAuthority's in-memory state, including next_id_, is destroyed with it; only the blob file and the ACL file survive on disk)
=== RUN 2 (read_side, separate process) ===
[read_side]  minted UNRELATED principal 'session-for-mallory' -> id=1
[read_side]  persisted ACL says digest fb54a9abf66287ada914c5c8dc50eb3d3836c0a4423e5d2c457d4368ed355777 was authorized for principal id=1 (Alice's principal, from the PREVIOUS process)
[read_side]  is Mallory's freshly-minted id the SAME numeric id Alice held? YES -- id was recycled across the restart
[read_side]  authorized_for(acl={1}, caller=Mallory id=1) = TRUE (ACCESS GRANTED)
[read_side]  RESULT: Mallory (an unrelated principal from a different process lifetime) READ ALICE'S SECRET CONTENT:
    "ALICE'S PRIVATE CHECKPOINT CONTENT -- must never be readable by an unrelated later principal"
[read_side]  This is a real, demonstrated cross-principal leak caused entirely by principal-id recycling across a process restart, using the project's own real authorized_for() ACL-check logic and real durable storage.
read_side exit=2
```

First run, no flake, on the second independent run just as on the first.

### 33.3 Why this is worse than "misuse `mint_root()` and get what you deserve"

Two things rule out "this only happens if someone does it wrong": first, Mallory required **no
malice, no crafted input, and no exploit of any check** — she is an ordinary, legitimate new session,
and the leak is a pure accident of counter timing, not an attack she chose to mount. Second, and more
importantly, this is not a hypothetical misuse of the low-level `mint_root()` API — the design's own
intended PRODUCTION bridge, `IdentityAuthority::adopt(real_id, on_behalf_of)` (§25, the mechanism
`RealSandboxReflector`/`SandboxReflector` actually call on every real tool invocation), allocates from
the exact same non-persistent `next_id_` counter and caches its string→id mapping in an equally
non-persistent `adopted_` map. Using this design's own recommended integration path, exactly as
documented, inherits the identical recycling bug the moment identity crosses a process restart.

### 33.4 Severity and fix direction

**This is a design gap, not (yet) a shipped code bug.** None of this document's own §20-32 probes
persist `blob_acl_`/`tree_acl_`/`branches_` anywhere — every probe's `IdentityAuthority` and every
probe's `Ledger` are born and die in the same process, so today's code fails CLOSED on a real restart
(fresh, empty maps mean every lookup is `not found` → denied), not open. The danger is specifically
that it fails WIDE OPEN the instant anyone builds the durable `Ledger` storage §11 already asks for,
without ALSO independently solving identity durability first — an ordering trap: the natural, locally
reasonable next step (persist the content store, since that's the piece with an obvious file-backed
implementation already sitting in `docs/planning/proofs/worktree_io/`) silently reintroduces exactly
the kind of cross-session leak §29's entire ACL mechanism exists to prevent, without touching a single
line the ACL fix itself.

**Fix direction** (a direction, not a decision — this needs its own design pass before being built,
per this project's own process for anything touching I2/I3-adjacent identity machinery): the id space
`IdentityAuthority` allocates from must itself become durable, and must do so BEFORE (or atomically
with) any Ledger/ACL structure keyed by its ids is made durable — either a persisted,
monotonically-increasing high-water-mark the allocator reads on `bootstrap()` before minting anything
new, or ids drawn from a non-recycling source that does not depend on process-local counter state at
all (e.g. a UUID, or a value derived from something already durable). Whichever is chosen, "durable
identity precedes durable authorization, never the reverse" should be stated as a hard precondition
in whatever design eventually closes §9's integration seam or §11's storage-durability question — not
left as an implicit assumption a future implementer could miss the way this document itself did for
32 sections.

### 33.5 What this red-team round does and does not establish

**Established**: a real, reproducible, twice-independently-verified architectural gap between this
design's two durability stories (content vs. identity) that no prior round — 4 rounds of textual
red-team, an extensive real-compiler prove phase, a real adversarial attack simulation, and a
meta-review of the prove phase's own code — happened to name, because every one of them tested
identity and durability as SEPARATE concerns and never composed them; and that this gap sits
precisely on the seam between one already-named open question (§11's durable storage) and one
already-proven mechanism (§29's ACL fix), which is exactly the kind of composition seam this
document's own history (§26, §32) has repeatedly shown is where real bugs actually live.

**NOT established**: whether this is the ONLY such composition seam remaining (this was one
deliberately-scoped red-team round picking one issue, not an exhaustive sweep of every possible
pairing of this design's now-numerous proven-in-isolation properties); a concrete, judged design for
durable identity (§33.4 names a direction, not a specification); and whether `IdentityAuthority`'s
already-disclosed "trusted native code, not cryptographic" threat model (§16/§19.1) is even the right
frame for reasoning about identity durability, or whether durable identity needs its own, separate
threat-model statement — an open question this round surfaced but did not attempt to answer.

## 34. Closing every remaining open question — real design, real code, real re-verification at every step

Prompted by an explicit instruction to resolve every item §11 and the scattered "NOT established"
notes across §20–§33 still named as open, done with the same explicit worry this document's own
history justifies: three separate times already (§26↔§29, §32.1, §33.1), independently-plausible
pieces of this design quietly diverged into two non-overlapping artifacts before anyone noticed. A
plan was written and independently pressure-tested (a second, fresh agent critiqued the proposed
dependency ordering before any code was touched) specifically to reduce that risk — not just to
close the items faster. The full inventory, the final agreed ordering, and the critique that shaped
it are preserved outside this document in the session's own planning record; what follows is the
real, executed result, checkpointed with a full probe-suite re-run after every high-risk step, not
only at the end.

### 34.1 Divergence audit — a clean baseline, confirmed before any new code

Before touching anything: every `class`/`struct` name and every free function across the whole
`docs/planning/proofs/` tree was enumerated and checked for near-duplicates beyond the three already
known and documented (§26.1/§32/§33's own `Ledger`/`BranchHandle`/`Checkpoint`/`BranchState`/
`MediatedFileSystem`/`SandboxSession`/`StagedWrite` pairs, each a deliberate, separately-scoped,
self-consistent artifact for its own probe, not a hidden accident). None found. All 22 existing
probes (17 positive, 5 negative-must-fail-to-compile) were recompiled and re-run to completion as an
explicit starting baseline — **ALL CHECKS PASSED, FAIL=0** — before any of A1–A8/B1/B2 began.

### 34.2 A1 — `IdentityAuthority` id durability (closing §33's own finding)

Design: `IdentityAuthority::bootstrap()` gains one new, entirely optional parameter,
`std::optional<std::filesystem::path> durable_dir = std::nullopt`, mattering only on the very first
call in a process (Meyer's-singleton "first call wins," the same shape this design already accepts
elsewhere) — every existing call site (`bootstrap()` with no argument, all 22 prior probes) is
byte-for-byte unaffected. When given a directory, the id high-water-mark (`identity_next_id.txt`)
and the `adopted_` real-principal bridge (`identity_adopted.log`, append-only) are both made durable:
a brand-new principal can never receive a previously-issued id back (closing §33's leak), and a
previously-`adopt()`-ed real principal reliably gets its OWN id back across a restart (so the fix
does not trade the security hole for "every legitimate session loses its own access on every
restart," which §33.4's own fix direction explicitly did not intend). `next_grant_id_`/`Grant<T>`
durability is explicitly out of scope — nothing anywhere persists a `GrantSet` or a grant-keyed ACL,
so there is no matching leak for it to close yet.

Re-running §33's own exact two-process scenario against the fix, real output, first run:

```
=== RUN 1 (write, separate process) ===
[write] noise1 id=1, noise2 id=2, alice id=3 (durable dir=ae_durable_identity_probe)
[write] (this process now exits -- IdentityAuthority's in-memory state dies with it; only
identity_next_id.txt and identity_adopted.log survive on disk)
=== RUN 2 (read, separate process) ===
[read]  mallory id=4
[read]  mallory.id() > 3 (no recycling across the restart): PASS
[read]  alice re-adopted id=3 (must be 3, her original id from process 1)
[read]  alice_again.id() == 3 and != mallory.id() (durable re-adoption preserves her own
legitimate identity across the restart, without granting it to anyone else): PASS
[read]  is Mallory's id the SAME as Alice's persisted id 3? no -- fix holds

ALL CHECKS PASSED -- the real, twice-reproduced §33 leak (id recycling across a genuine process
restart) no longer occurs, and the legitimate principal's own continued access to her
durably-owned content is preserved, not sacrificed to fix it.
```

Checkpoint: the full 22-probe baseline suite recompiled and re-run — **FAIL=0**.

### 34.3 A2 + A8 — a durable `Ledger`, with a bounded ACL baked into the same design step

Per the independent Plan-agent critique that shaped this whole pass's ordering: A8 (the per-digest
ACL growth bound, §29.6) was folded INTO A2's own design, not sequenced as a follow-on — persisting
an already-known-unbounded `std::set<uint64_t>` and then having to migrate the format again to add a
bound would have been exactly the kind of avoidable rework the critique flagged.

`Ledger` became `Ledger<Store = agentengine::InMemoryWorktreeObjectStore>` (and `BranchHandle<Store>`
alongside it) — deliberately NOT a second, parallel `DurableLedger` type. The whole reason to
templatize rather than fork is that §26/§32/§33 each independently found real bugs caused by two
independently-plausible implementations quietly diverging; one `Ledger`, parameterized on its object
store, makes "the durable configuration" and "the in-memory configuration" be, structurally, THE SAME
CODE. Every existing `Ledger ledger;` / `Ledger shared_ledger;` call site across the whole tree is
completely unaffected — C++'s class-template-argument-deduction rules (verified with a real, minimal
standalone compile check before touching the real file) resolve the bare name to the default
`InMemoryWorktreeObjectStore` automatically, both for default-construction and for move-construction
from an already-concrete value. `BranchState`/`Checkpoint` had their `Principal authored_by`/
`created_by` fields simplified to plain `authored_by_id`/`created_by_id` (`uint64_t`) — a full grep
audit confirmed nothing anywhere ever read more than `.id()` from either field, so this made durable
serialization trivial without inventing any new "reconstitute a Principal for an id I already know
about" capability on `IdentityAuthority`.

`branches_`/`blob_acl_`/`tree_acl_` are now durable atop the already-durable `FileWorktreeObjectStore`
(§28.2) via a full-snapshot rewrite (temp file + atomic `rename`) after every mutation — deliberately
NOT an append-only event log (simpler to review, no replay/compaction logic to get subtly wrong), at
a real, disclosed, UNMEASURED I/O cost proportional to total state size per mutation (the same honest
posture §11's own MediatedFileSystem-performance question already carries). Crash-safety scope
matches §28.2's own bar exactly: safe across a clean exit or crash, not across a genuine power loss
mid-write. A8's bound (`kMaxAclRootsPerDigest = 64`, a deliberately generous, documented-not-tuned
default) is enforced at every insertion point and fails CLOSED — a new, distinct root past the cap is
rejected (`ledger.acl_root_cap_exceeded`), never silently dropped and never silently evicting an
existing, still-legitimate grant (eviction was explicitly rejected as an option: it would silently
revoke real access, which is worse than a clear, attributable rejection of new growth). A7's eventual
crash-reservation record was sketched into this same design step (documented below in §34.6) so its
real implementation would not need a second migration of the durable format either.

Real, twice-verified proof (two separate OS processes), first run:

```
=== WRITE (process 1) ===
[write] owner id=1
[write] branch=root-1 turn1_tree=b46e8e05... turn2_tree=29b458d1...
=== READ (process 2) ===
[read] owner re-adopted id=1
[read] restored branch=root-1 head_tree=29b458d1...
[read] (1) restored tree has both real files; a.txt content recovered off real disk through the
restored Ledger: "durable ledger content, turn 1" -- PASS
[read] (2) the PERSISTED turn-1 checkpoint survived the restart intact (1 file, not 2) -- real
checkpoint HISTORY, not just the latest head, was durably restored -- PASS
[read] (3) an unrelated outsider principal minted fresh in THIS process is still REJECTED against
the RESTORED ACL (tree: ledger.tree_access_denied, blob: ledger.blob_access_denied) -- PASS

ALL CHECKS PASSED
```

A8's bound, real output, first run:

```
[1] admitted exactly 64 distinct root principals into one digest's ACL: all succeeded
[2] the 65-th distinct root was REJECTED (ledger.acl_root_cap_exceeded)
[3] re-touching the SAME content from an ALREADY-admitted root, at the cap, still succeeds -- PASS
[4] every one of the 64 originally-admitted roots can still read the content after the cap was
hit -- the bound rejects NEW growth, it never revokes existing, already-legitimate access -- PASS
[5] the rejected overflow root correctly cannot read the content -- PASS
```

Checkpoint: the full probe suite (now 24 probes) recompiled and re-run — **FAIL=0**.

### 34.4 A4 — wiring the real `merge_trees()` into `Ledger::merge()`, and a real vulnerability this pass found and fixed along the way

A fourth instance of this document's own recurring pattern: `merge_trees()` (§28.3, a real, tested
three-way merge with conflict detection) was, before this step, called by NOTHING except its own
standalone probe — confirmed by grep, zero other references anywhere. `Ledger::merge()` (both the
pre- and post-§32 version) was a pure fast-forward stub. `BranchState` gained one new durable field,
`base_tree_digest` (the parent's tree at the exact moment a branch was created — the real three-way
merge's `base`), and `merge()` now loads `base`/`ours` (parent's current tree)/`theirs` (child's
current tree) for real and calls `merge_trees()`. A real conflict now FAILS the merge closed
(`ledger.merge_conflict`) rather than silently picking a side, matching §11's own "resolution UX is
out of scope, detecting a conflict correctly is not" boundary. Every entry in a clean merge result is
validated against the same per-entry authorization `commit()` already requires, so a merge cannot
smuggle unauthorized content into the parent's history any more than a direct commit can.

**Writing the real attack probe for this new surface (§34.7 below) found a genuine, exploitable
vulnerability in this exact change before it ever shipped as "done":** the per-entry authorization
check above was added for a narrower reason (preventing content smuggling), and it happened to
incidentally block one shape of a different attack — but not all shapes. See §34.7 for the full
finding and fix; `merge()`'s final, shipped signature (`merge(BranchHandle<Store> child,
BranchHandle<Store> const& parent, Principal requested_by)`, parent now a possessed handle, not a
bare string) already reflects that fix, and every real-usage example below uses it.

Real output, both a clean merge and a real conflict, through `Ledger::merge()` itself (not only the
standalone pure function), first run:

```
[1] root branch, turn 1: a.txt="A-original" committed
[2] REAL CLEAN MERGE: a.txt correctly took the CHILD's change, b.txt correctly kept the PARENT's
change -- a real three-way merge combined two divergent, non-overlapping edits, not a fast-forward
wholesale adoption -- PASS
[3] REAL CONFLICT correctly REJECTED (code=ledger.merge_conflict): both sides changed a.txt to
different content from a common base -- merge does NOT silently pick a side
[4] the child branch was NOT erased by the rejected merge -- still resolvable for a real caller to
retry or explicitly abandon -- PASS

ALL CHECKS PASSED
```

Checkpoint: the full probe suite (now 25 probes) recompiled and re-run — **FAIL=0**.

### 34.5 A5 — a store-wide quota ceiling above every session's own subtree

Closes §11's "cross-session/store-wide quota interaction" open question with a DECISION, not a new
mechanism: a real deployment mints exactly ONE root `AsyncQuota<StorageBytes>` per STORE, and every
session's own quota is obtained via the already-proven (§21) `allocate_child_share()`, never a second
independent `mint_root()` call. §5 already specifies `AsyncQuota<T>` as "scoped per Principal
subtree" — the store-wide ceiling is simply the ROOT of that same subtree. This needed no new code
in `async_quota.hpp` at all, only a real demonstration that it produces the missing property.

Real output, first run:

```
[1] two sessions each granted a 400-byte child share off the ONE store-wide root (1000 total):
root remaining=200 (expect 200)
[2] a THIRD session requesting 300 bytes is REJECTED (insufficient remaining budget to allocate
child share) even though 300 is a perfectly reasonable per-session limit on its own -- the
STORE-WIDE ceiling, not any individual session's own limit, is what's actually exhausted. A
per-session-independent mint_root() model (the historical design's own rejected approach) could
never produce this rejection at all.
[3] session-3 requesting exactly the remaining 200 bytes succeeds; root remaining is now 0
[4] all three sessions can still fully spend their own already-granted child shares even though
the store-wide root itself is now fully allocated -- PASS

ALL CHECKS PASSED
```

### 34.6 A7 — what happens to a `BranchHandle` across a process crash (a real implementation simpler than its own sketch)

§34.3's own sketch proposed a lease/expiry-based reservation record needing a heartbeat renewal
mechanism. Implementing it for real found that unnecessary: a `BranchHandle` is a plain in-process
C++ object that categorically cannot survive a process exit, clean or crashed. Therefore EVERY branch
`load_durable_state()` restores is, by construction, orphaned relative to the new process — no timing
window or lease-expiry judgment call needed, a real simplification found only by attempting the
implementation, disclosed here rather than force-fitting the original sketch. Reclaiming is
deliberately NOT automatic (no silent abandon-on-restart): a host inspects `orphaned_branches()` and
explicitly calls `reclaim_orphaned_branch()` (mints a fresh, live handle) or
`abandon_orphaned_branch()` (discards it) — both gated by the SAME `authorized_for()` check every
read already uses, so an unrelated caller cannot reclaim a stranger's orphaned branch merely by
knowing its name.

Real, two-process proof — process 1 commits real content then exits with its branch handle never
released (a real crash simulation, not a contrived one); process 2 restarts and reclaims:

```
[read] (1) branch 'root-1' correctly appears in orphaned_branches() after the restart -- PASS
[read] (2) an UNRELATED principal's reclaim attempt was REJECTED (ledger.reclaim_unauthorized) --
knowing the branch's name is not enough -- PASS
[read] (3) the legitimate owner successfully reclaimed the orphaned branch -- a real, fresh, live
BranchHandle was minted -- PASS
[read] (4) a real commit through the reclaimed handle succeeded (turn_index=2, continuing the
branch's real pre-crash history) -- PASS
[read] (5) a second reclaim attempt on the SAME (now-live) branch was correctly REJECTED
(ledger.not_an_orphan) -- no double-reclaim -- PASS

ALL CHECKS PASSED
```

Checkpoint (batched, per the critique's own "lower-risk/additive items don't need separate
checkpoints"): the full probe suite (now 27 probes) recompiled and re-run — **FAIL=0**.

### 34.7 B1 — red-teaming `branch_from()`/`merge()` for the first time ever, and a real vulnerability found and fixed

§32.6 named this explicitly: `branch_from()`/`merge()` had zero adversarial coverage, the newest and
least-scrutinized surface in the whole document. The first real attack attempt found a genuine,
exploitable vulnerability: `merge()` took its parent argument as `std::string const& parent_name` — a
bare, GUESSABLE string (branch names follow the deterministic `"root-<owner_id>"` scheme) — with NO
possession check on the parent side, unlike every other mutating `Ledger` method (`commit`/
`reset_to`/`abandon` all require the caller to already HOLD the actual handle for whatever they act
on). Against a victim branch with existing content, an incidental side effect of the per-entry
authorization check added in §34.4 happened to block the attack (the merged result would include the
victim's own pre-existing entries the attacker isn't authorized for) — but against a freshly-created,
still-EMPTY victim branch, that check has nothing to trip, and the attack went straight through. Real,
first-run, pre-fix output, captured for the record:

```
[setup] victim's EMPTY branch created: root-1, head=df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119
[ATTACK] SUCCEEDED -- victim's EMPTY branch corrupted! new turn_index=1 tree=9ae28a68b4d6bfcbe27a14928a345875e55e576b3f66e34d8c2fc6d697f04c60
```

This is precisely the shape of bug this document's own methodology exists to catch: a plausible-
looking, partially-effective check (the per-entry authorization) created a false sense that the
surface was covered, and only a genuine, adversarial attempt against the WEAKEST variant (an empty
victim) revealed it wasn't. Fixed by requiring possession of the parent's own `BranchHandle` too —
`merge(BranchHandle<Store> child, BranchHandle<Store> const& parent, Principal requested_by)` — so
resolving which branch to merge into now depends on ALREADY HOLDING it, structurally, the same way
every other mutating method already works; naming a target by string is no longer even something the
type system allows, not merely something runtime logic happens to reject. Real, post-fix output:

```
[1] the attacker CAN merge their own child into their OWN possessed root -- that is legitimate,
ordinary use, not an attack -- PASS
[2] the victim's branch head is UNCHANGED -- the attacker never had a `BranchHandle` for it and
merge() no longer accepts a bare name, so the original attack shape cannot even be EXPRESSED
anymore, let alone attempted -- PASS
[3] the VICTIM's own legitimate merge (into a branch they actually possess) still works correctly
after the fix -- PASS

ALL CHECKS PASSED
```

### 34.8 B2 — re-stressing the real ACL fix under genuine concurrency, with A1/A2/A4/A8 all composed

§29.6's own disclosed gap: the ACL fix (§29) had never been re-stress-tested under real concurrent
read/write contention, either when it was first added or after §32's Ledger unification. Many real OS
threads, sharing ONE `Ledger`: 8 owner principals concurrently writing and reading back their own
content (1,600 rounds total), a concurrently-running attacker repeatedly targeting REAL, just-published
digests belonging to other owners (a real leaked/observed digest, not a guessed one — the strongest
realistic version of this attack), and 8 owners concurrently committing real trees through the bounded
ACL insert path (A8) at the same time. Run three times to rule out flakiness. Real output (one run):

```
[1] 8 owners x 200 rounds, concurrent writes+reads of their OWN content: write_ok=1600
write_fail=0 read_ok=1600 read_fail=0
[2] attacker threads made 2130 attempts against REAL, just-published digests belonging to other
owners, running concurrently with the owners' own writes: denied=2130, LEAKED=0
[3] 8 owners x 20 concurrent commits each (real bounded-ACL insert path, A8, under real
contention): commit_ok=160 commit_fail=0

ALL CHECKS PASSED -- zero leaks across 2130 real attacker attempts, zero lost/corrupted legitimate
writes or reads, zero bounded-ACL-insert failures under real concurrent commits.
```

Zero leaks across all three runs (attempt counts varied by real thread-scheduling timing — 1,992,
2,579, and 2,188 attempts respectively — the ZERO-leak result did not).

### 34.9 Final convergence pass

Per the critique's own emphasis on not repeating §26/§32/§33's mistake (checkpointing only at the
very end): the divergence audit from §34.1 was repeated in full after every item above landed — same
result, the same three already-known, already-documented pairs, no new one. The complete probe suite
(28 positive probes, 5 negative-must-fail-to-compile probes) was recompiled and re-run to completion
one final time — **ALL CHECKS PASSED, FAIL=0** — and the out-of-scope `docker_sandbox/
probe_docker_sandbox.cpp` (a real dependent of `worktree_ledger.hpp` even though outside this pass's
own review scope) was separately confirmed to still compile clean against every change.

### 34.10 A3 and A9 — explicitly out of scope, not silently dropped

**A3** (concrete execution-surface technology, §11 item 2, this document's own "single largest
remaining engineering unknown") remains deliberately untouched. It overlaps the engine's existing,
separately-Judged `native_jail`/`wasm`/`kata` backends, and the working assumption is that
`SandboxSession`/`MediatedFileSystem` will end up composing with one of those rather than a fourth
backend appearing beside them — but that is a preference for A3's own eventual implementation
decision, not a constraint on the design/prove work itself. Reuse-of-existing-machinery only
becomes a real concern once A3 is actually being wired into the live engine; until then, whatever
is cleanest to design and prove standalone — a new primitive, or a rewrite of an existing one — is
in scope, exactly as the rest of this document's own design/prove work has already been conducted.

**A9** (real integration into `AgentSession`/`ContextProvider`/`Tool<>`, §9) remains deliberately
untouched: §9's own text already states this needs its own design → red-team → prove → judge pass and
ADR. The confused-deputy residual named in §29.4 (host code itself could be tricked into targeting
the wrong object even though object-possession blocks a direct attack) is explicitly INHERITED by
whatever design closes A9, not dropped by omission here.

**A stated dependency, made explicit rather than left implicit** (the exact kind of gap this
document's own history — §26, §32, §33 — has repeatedly shown hides in unstated assumptions): §34.3's
durable `Ledger` and §34.6's crash-reclaim mechanism both assume the execution surface runs as a
process/lifetime independent of the Ledger's own storage process — consistent with §33's own
two-process finding, and something A3/A9's eventual design must honor, not silently violate.

### 34.11 What this pass does and does not establish

**Established**: every item named in §11's original "Open questions" list, and every scattered "NOT
established" note across §20–§33 that was actually actionable (as opposed to a disclosed, permanent
threat-model boundary like Attack 5 or the TOCTOU symlink residual), is now closed with real,
first-run-passing, twice-or-more-verified code — not merely documented as decided. A real,
exploitable vulnerability (§34.7's merge-by-name gap) was found by this pass's own adversarial
step and fixed before ever being reported as done, the fifth real bug this document's overall
methodology has caught that a textual read-through did not. The full probe suite converged — checked
explicitly, repeatedly, not merely hoped — with zero new divergence introduced across eight
substantial, composed changes to the same shared `Ledger`/`IdentityAuthority` surface every other
probe in the tree depends on.

**NOT established**: whether A2's full-snapshot-rewrite persistence strategy is performant enough
for any real deployment shape (explicitly disclosed as unmeasured, matching §11's own pre-existing
MediatedFileSystem-performance question); whether `kMaxAclRootsPerDigest`'s specific value (64) is
right for any real deployment (a deliberately generous, documented-not-tuned default); A3 and A9
themselves, named above as explicitly deferred, not solved; and — stated with the same honesty this
document has applied to itself every time before — whether this is truly the LAST composition seam
in a design that has now had real bugs found at its composition points four separate times (§26,
§32, §33, §34.7 itself). This pass was thorough and included a dedicated divergence audit
specifically because of that history, not because the history is now assumed closed.

### 34.12 An unrelated, real finding surfaced by this pass's own final verification sweep — root-caused and fixed by three dedicated follow-on investigations

While re-running the complete probe suite as this pass's own §34.9 final convergence check, one
PRE-EXISTING probe this pass never touched — `async_quota/probe_positive.cpp`'s own real
multi-threaded contention test (§21's own "Part B," first written well before A1) — failed
intermittently: roughly 1 run in 15–20, `q2->remaining()` came back 1 higher than the independently
tracked `expected_remaining`. First disclosed here as an open, unpinpointed finding (no
ThreadSanitizer support exists for this environment's `x86_64-pc-windows-msvc` clang target). Per an
explicit instruction to keep investigating, three independent, parallel follow-on investigations were
run — root-causing the mechanism, assessing real production blast radius, and checking whether the
same pattern generalizes to `channel<T>`/`task<T>` — and their combined result closed this for real,
not by more reasoning but by a real ASan-confirmed repro and a real, re-verified fix.

**Root cause (confirmed via a real, instrumented ASan repro, not speculation).** Compiling the
unmodified real probe with `-fsanitize=address -g -O0` reproduced a symbolized heap-use-after-free on
5/5 immediate attempts; a non-ASan stress at reduced scale still crashed 17/30 runs — the ~5–7%
"silent wrong value" symptom this section originally reported was the rare SURVIVABLE subset of a
much more frequently occurring memory corruption, not the whole picture. The exact mechanism: the
original `docs/planning/proofs/common/block_on.hpp`'s `drive_and_signal()` set its completion flag as
an ordinary body statement (`state->set(std::move(v)); co_return;`) one step BEFORE its own coroutine
frame actually finished unwinding (local-variable teardown, `final_suspend()`). Because
`AsyncMutex::unlock()` (deliberately, correctly, per that file's own banner) resumes a parked
coroutine on WHATEVER thread calls `unlock()` — not necessarily the thread that started it — the
polling thread calling `block_on()` could observe the flag and destroy the driver coroutine's frame
while a DIFFERENT thread was, at that exact instant, still physically executing inside that same
frame. Two independent ASan traces caught both variants of this directly (a read inside
`task<void>::promise_type::FinalAwaiter::await_suspend` racing a free from
`drive_and_signal`'s own local-teardown on a different thread, and the reverse). An instrumented
"believed-holders" counter confirmed `AsyncMutex`'s own mutual-exclusion invariant was NEVER violated
(max observed: exactly 1, across every completed run) — the bug was entirely in `block_on.hpp`'s own
driving pattern, not in `agentengine::rt::AsyncMutex` or `agentengine::rt::task<T>` themselves, both
confirmed intact by this investigation.

**Generalization check (confirmed NOT to spread to `channel<T>` or `task<T>` itself).** `channel<T>`
has the structurally identical "release internal lock, then `.resume()` outside it" hand-off shape
(`include/agentengine/rt/channel.hpp`'s `push()`/`finish_terminal()`) — already documented by
`async_mutex.hpp`'s own banner as sharing the SAME cancellation-safety residual — but is strictly
single-producer/single-consumer with no FIFO multi-waiter queue, so it cannot reproduce the N-way
chained-hand-off shape this bug needs, and no real usage of `channel<T>` in this codebase puts it in
that N-way shape today. A direct trace of `task<T>`'s own promise fields (`continuation_`, inline
result storage) across every real cross-thread hand-off found each one properly bridged by a real
`std::mutex` release→acquire pair at the exact suspend/resume boundary — no evidence the shared
coroutine substrate itself is at fault, consistent with the root-cause finding pointing squarely at
`block_on.hpp`.

**Blast radius (confirmed not live in any shipped production path today).** Every real production
usage of `AsyncMutex` (`AgentSession::session_mutex_`, `WorkflowSupervisor::run_mutex_`,
`ProjectRegistry::mutex_`, `SpawnCostBudget::mutex_`) was checked directly: no shipped host/dispatch
code drives genuinely concurrent OS threads into the same instance today.
`agent_workflow_executor.hpp` states explicitly that its own driving loop is safe ONLY because
`WorkflowSupervisor` never dispatches two concurrent deliveries to the same executor within one
round; `tools/cli_chat.cpp` drives one `AgentSession` turn at a time through a single worker;
`thread_pool.hpp` explicitly EXCLUDES genuine cross-thread `AsyncMutex` contention from its own
scope, naming the real multi-`std::thread` pattern as one a caller needing that must use *instead of*
`ThreadPool`; and ADR-061 §13.9 names multi-caller/thread-pool-dispatched sessions an explicit,
still-unbuilt non-goal. This bug therefore could not fire in any shipped path today — it would become
live the moment a real host-provided multi-threaded transport (the still-unbuilt ADR-061 Tier 3
listener, or any thread-pool-based host dispatcher routing concurrent requests into one session) ships,
which `AsyncMutex` is explicitly designed to make safe. `background_task()`'s detached thread was
checked directly and confirmed NOT a trigger — its completion hand-off deliberately never touches
`session_mutex_`, using its own separately-synchronous queue instead, precisely to stay off
`AsyncMutex`.

**Fixed — in `docs/planning/proofs/common/block_on.hpp` only, a prove-phase test harness, not real
`include/agentengine/rt/` production code**, so this fix does not touch anything CLAUDE.md's
design→red-team→prove→judge gate would apply to. `drive_and_signal()` now returns a minimal,
purpose-built local coroutine type (`detail::SignalTask<T>`) instead of `agentengine::rt::task<void>`,
whose `final_suspend()` awaiter's `await_suspend()` performs the completion signal as the literal
LAST instruction ever executed on that frame — strictly after every local variable (the by-value
`inner` parameter, the locally-`co_await`-ed result) has already been destroyed, the standard
"cppcoro `sync_wait`"-shaped fix for exactly this problem. Real production `task<T>`/`AsyncMutex`
were not modified.

Real, re-verified results after the fix:
- The exact bug-exposing probe, rebuilt under ASan: **40/40 clean runs** (0 failures), versus 17-25/30
  crashing before the fix.
- The same probe, normal (non-ASan) build, the original reporting configuration: **40/40 clean runs**
  (0 failures), versus the originally-observed ~5-7% flake rate.
- The two OTHER pre-existing probes that also share one `AsyncQuota`/`AsyncMutex` across genuinely
  concurrent real OS threads via `ledger.commit()` (`worktree_io/probe_concurrent_ledger.cpp`,
  `worktree_io/probe_concurrent_io.cpp`, both from §28.4, both silently exposed to the identical
  UAF all along without ever having tripped a visible symptom in their own assertions): **20/20 clean
  runs under ASan** (10 each) after the fix.
- The complete probe suite (28 positive, 5 negative-must-fail-to-compile) recompiled and re-run in
  full: **FAIL=0**, zero regression from the `block_on.hpp` change.

### 34.13 What the AsyncMutex investigation round does and does not establish

**Established**: a real, ASan-confirmed, previously-undiscovered use-after-free in a test harness
(`block_on.hpp`) used by roughly half the multi-threaded probes in this entire document, root-caused
to an exact mechanism (signaling completion before a coroutine frame's own teardown, under genuine
cross-thread resume) rather than left as an unpinpointed intermittent flake; that the engine's real,
shared `agentengine::rt::AsyncMutex` and `agentengine::rt::task<T>` substrate is NOT at fault (their
own invariants were independently, empirically confirmed intact); that `channel<T>` does not share
this exposure today; that no shipped production code path can trigger this bug as things stand,
because the codebase's own current concurrency model (ADR-061 §13.9, `thread_pool.hpp`,
`agent_workflow_executor.hpp`) does not yet dispatch genuinely concurrent OS threads into one
`AsyncMutex`-guarded instance anywhere; and that the fix is real, contained to prove-phase code, and
re-verified under the same tool (ASan) that caught the original bug, not merely re-run until it
happened not to fail.

**NOT established**: whether `AsyncMutex`/`task<T>` themselves would hold up under ThreadSanitizer
specifically (unavailable on this environment) rather than ASan plus manual instrumentation — the
confirmation here is strong but not from every tool that could in principle catch a subtler variant;
and — the load-bearing point for whoever eventually opens the ADR-061 Tier 3 / ADR-061 §13.9
multi-caller work — that this was the ONLY latent hazard waiting in that direction. This investigation
was thorough about the ONE bug it found, not a general clearance of "the coroutine substrate is ready
for genuine multi-threaded session dispatch," which nothing here claims and ADR-061 itself does not
yet attempt.

## 35. Second code review of the prove phase — 10 real findings, fixed and re-verified

A second independent adversarial code-review pass (fresh agent, no memory of this document's own
narration) was run against `docs/planning/proofs/`, this time including `docker_sandbox/` (§31 had
its own separate review at the time; this pass covers it too). Mandate: find real bugs, not style
issues, ranked by severity. It found 10, none previously named anywhere in this document. Every one
was fixed in code and re-verified by recompiling and re-running the actual probes each fix touches
(`clang 22.1.5`, target `x86_64-pc-windows-msvc`, `-std=c++23`) — not merely re-read.

### 35.1 The findings, most severe first

1. **`AsyncQuota<T>::try_consume()` silently discarded its own spender-identity check**
   (`async_quota.hpp`) — the design's own spec (§13.3, this document's line ~292: "fails closed ...
   if spender is not this quota's owner or a descendant it was split to") was never implemented; the
   parameter was discarded via `(void)spender;`. Under the store-wide-ceiling deployment pattern
   §34/A5 itself recommends (one shared root quota, every session a `derive_child()`), ANY session
   holding a reference to that shared quota could drain any OTHER session's budget by passing any
   `Principal` — a real I2/I8 violation once multiple tenants share one quota, not a theoretical one.
2. **`RealIoFileSystem::materialize()` had no path-safety check on Tree entry names**
   (`real_io_filesystem.hpp`) — `write()`/`read_real_file()` both call
   `reject_unsafe_relative_path()`/`reject_symlink_escape()` before touching disk; `materialize()`
   (the rollback path) did not, and `Ledger::commit()` only ever ACL-gates an entry's *digest*, never
   its *name*. A committed entry named `"../../evil.txt"` would escape the sandbox root on rollback.
3. **`Ledger::merge()` permanently stranded the child branch on every rejection path** — a real
   merge conflict (an *expected* outcome, not just an adversarial one, per §11's own explicit
   "conflict resolution is out of scope" boundary) set `child.resolved_ = true` on rejection, which
   suppresses `BranchHandle`'s destructor-time `maybe_queue_abandon()`, with no registration as an
   orphan either — leaving the branch present in `branches_` (readable) but with no live handle
   anywhere and no way back into it via `reclaim_orphaned_branch()`/`abandon_orphaned_branch()`
   (both correctly reject it as "not a recognized orphan"). `probe_ledger_merge.cpp`'s own Scenario 2
   comment claimed the branch was "still there for a real caller to retry or explicitly abandon" —
   true only for reads, never actually demonstrated for a real handle back.
4. **Quota consumed before validation, with no refund on failure** — `commit()`/`branch_from()`
   both call `quota.try_consume()` before the branch/ACL checks that can still reject the operation,
   with no failure path refunding what was already deducted. A caller whose commit is rejected for
   an unrelated reason (stale branch, unauthorized reference) permanently loses budget for zero
   stored content — a self-inflicted or adversarial quota-exhaustion DoS.
5. **`materialize()` and `write()` used two different, unrelated locks** on the same real directory
   tree (`commit_lock_` vs. `sync_mutex_`) — never mutually exclusive despite both mutating
   `host_root_`; a concurrent `write()` could land mid-`remove_all()` or mid-rewrite.
6. **`IdentityAuthority::persist_high_water_mark()` was not crash-atomic** — a plain truncating
   `ofstream`, unlike `worktree_ledger.hpp`'s own `persist_snapshot_locked()` (temp file + atomic
   rename) built for exactly this reason. Since `allocate_id()` calls this on *every* mint, a crash
   mid-write leaves a truncated file `load_durable_state()` cannot distinguish from "no file yet,"
   silently resetting `next_id_` to 1 and re-issuing already-live principal ids — directly
   undermining A1's own durable-identity goal (§33/§34).
7. **`FileWorktreeObjectStore::decode_tree()` had no bounds checking; `put_tree()`/`put_blob()`
   were not crash-atomic** — a crash mid-write leaves a truncated file at the final digest-named
   path; a later `get_tree()` on that digest then drove `decode_tree()` past the end of the byte
   vector (undefined behavior), not a clean error.
8. **`Ledger::checkpoint_at()`/`head_tree_digest()` performed no identity check at all** — every
   other Ledger accessor (`get_blob_safe`/`get_tree_safe`) gates on `authorized_for()`; these two did
   not, and since branch names are deterministically guessable
   (`root-<owner_id>`/`<parent>/child-<id>-<seq>`), any caller could enumerate an arbitrary branch's
   current head digest, and — worse, since `checkpoint_at()` returns `authored_by_id` and turn-index
   history, not merely a digest — who authored each turn and how many turns occurred.
9. **Shell injection in `docker_backend.hpp`** — every method built a `cmd.exe`-interpreted command
   string by raw concatenation of caller-supplied values (image, host/container paths, `exec()`'s
   own `command`) with no escaping before `_popen`. A value containing a double-quote or a cmd.exe
   escape character could break out of the intended quoting and execute attacker-controlled commands
   on the HOST — defeating the exact isolation boundary this probe exists to demonstrate (§31).
10. **`combine_into_tree()`/`harvest_and_checkpoint()` had no rollback on a partial ACL-cap
    failure** (`real_sandbox_session.hpp`) — each staged write is durably persisted via
    `put_blob_safe()` in a loop; if the Nth blob's digest hit the ACL-root cap (A8), blobs `1..N-1`
    were already durably stored with no Tree/Checkpoint ever referencing them, and the caller saw
    only a clean error with no indication that partial content had already reached disk.

### 35.2 The fixes

1. `try_consume()` now checks `spender.id() == owner_.id() || children_.contains(spender.id())`
   before consuming, matching the spec exactly — a spender must be the quota's owner or a principal
   THIS quota instance itself split a share to via `allocate_child_share()`.
2. `materialize()` now runs every entry through `reject_unsafe_relative_path()` before writing
   anything, and `reject_symlink_escape()` per-entry right before each write (host_root_ having just
   been recreated by the rollback itself).
3. Every rejection path in `merge()` now calls `orphaned_from_restart_.insert(child.name())` before
   marking the handle resolved — reusing A7's own existing orphan-reclaim machinery (the same set a
   restart already populates) instead of inventing a parallel mechanism. `probe_ledger_merge.cpp`
   was extended to prove this for real: after a rejected conflict merge, the child branch now shows
   up in `orphaned_branches()`, `reclaim_orphaned_branch()` returns a genuinely fresh, live
   `BranchHandle`, and that handle can commit and be abandoned normally.
4. `commit()`/`branch_from()` now compute their outcome in a plain (non-coroutine) closure so
   `mutex_` is fully released before any `co_await` (this also fixes a second, independent latent
   bug the restructuring surfaced: holding a `std::lock_guard<std::mutex>` across a coroutine
   suspension point is undefined behavior if the coroutine resumes on a different thread than it
   suspended on), then call `quota.refund(amount)` — a new `AsyncQuota<T>` method, symmetric with
   the already-proven `release_child_share()` — on any failure.
5. `materialize()` now takes the SAME `sync_mutex_` `write()` uses around its own filesystem
   mutation, making the two genuinely mutually exclusive.
6. `persist_high_water_mark()` now uses the same temp-file-then-atomic-rename discipline as
   `worktree_ledger.hpp`'s `persist_snapshot_locked()`.
7. `decode_tree()`'s `read_u32()`/`read_str()` now check remaining length before every read and
   fail closed with `worktree.tree_decode_failed` instead of indexing out of bounds; `put_tree()`
   and `put_blob()` both now write to a `.tmp` sibling and atomically rename into place.
8. `head_tree_digest()` and `checkpoint_at()` both now take a `Principal caller` and return
   `result<T>` instead of a bare value/`optional`, gated through the same `authorized_for()` every
   other accessor uses (`checkpoint_at()` gates on the SPECIFIC checkpoint's own tree digest, not
   the branch's current head, so access to one historical state doesn't require — or imply — access
   to whatever the branch has since become). Five call sites across `full_stack/`, `attack_sim/`,
   and `worktree_io/` were updated to pass a real, already-in-scope `Principal`.
9. Two new guard functions reject (never attempt to escape) dangerous input before it reaches
   `_popen`: `reject_unsafe_for_shell()` (image/paths — none of `"&|<>^%` are ever legitimately
   needed there) and the narrower `reject_shell_breakout()` for `exec()`'s own `command` (which
   legitimately needs `&|<>` for the CONTAINER's inner shell; only `"`/`%`/`^` — the characters that
   actually defeat the OUTER cmd.exe quoting — are rejected). Explicitly disclosed as a NECESSARY,
   not sufficient, defense, matching this document's own established posture for
   `reject_symlink_escape()` — a fully general `cmd.exe` escaper is its own hard problem this fix
   does not claim to solve.
10. A new read-only `Ledger::would_accept_blob_write()` lets `combine_into_tree()` validate an
    ENTIRE batch of staged writes against the ACL-root cap before writing any of them, so a
    rejection is discovered before anything is written, not partway through.

### 35.3 Re-verification — every touched probe recompiled and re-run

Fixing (1) changed real behavior for two existing probes that had, until now, unknowingly relied on
the missing check: `worktree_io/probe_concurrent_io.cpp` and `worktree_io/probe_concurrent_ledger.cpp`
both had every session spend directly from a shared ROOT quota via a `derive_child()`'d principal
that was never granted a share of it — exactly the gap (1) closes. Both were fixed to allocate a real
per-session child share first (the same §34/A5 store-wide-ceiling pattern `probe_store_wide_ceiling.cpp`
already established), then re-verified. `ledger/probe_positive.cpp` (the OLDER, still-compiled Ledger
that shares the same `async_quota.hpp`) had the identical gap in two places (steps 8 and 10) — fixed
the same way. `attack_sim/probe_internal_attacks.cpp`'s own Attack 2 had, by accident, been spending
from the VICTIM's quota as the attacker — also blocked by fix (1), but for the wrong stated reason;
given the attacker its own legitimate quota so the probe's narrative (the tree-content ACL check,
not the quota check) matches what actually blocks it again.

Real re-run output, this pass, every probe (28 positive + 5 negative-must-fail-to-compile), full
recompile from clean object files:

```
async_quota/probe_positive.exe                 -- ALL CHECKS PASSED
async_quota/probe_store_wide_ceiling.exe       -- ALL CHECKS PASSED
worktree_io/probe_worktree_io.exe              -- ALL CHECKS PASSED
worktree_io/probe_concurrent_io.exe            -- ALL CHECKS PASSED (after fix 1's probe update)
worktree_io/probe_durability.exe               -- ALL CHECKS PASSED
worktree_io/probe_path_traversal.exe           -- ALL CHECKS PASSED
worktree_io/probe_acl_bound.exe                -- ALL CHECKS PASSED
worktree_io/probe_concurrent_acl.exe           -- ALL CHECKS PASSED
worktree_io/probe_concurrent_ledger.exe        -- ALL CHECKS PASSED (after fix 1's probe update)
worktree_io/probe_merge.exe                    -- ALL CHECKS PASSED
worktree_io/probe_ledger_merge.exe             -- ALL CHECKS PASSED (extended: reclaim proof, see 35.2.3)
identity_authority/probe_positive.exe          -- ALL CHECKS PASSED
identity_authority/probe_concurrent_adopt.exe  -- ALL CHECKS PASSED
identity_authority/durable_restart_write.exe -> durable_restart_read.exe (real 2-process restart) -- ALL CHECKS PASSED
worktree_io/durable_ledger_write.exe -> durable_ledger_read.exe (real 2-process restart)          -- ALL CHECKS PASSED
worktree_io/crash_reclaim_write.exe -> crash_reclaim_read.exe (real 2-process restart)             -- ALL CHECKS PASSED
attack_sim/probe_attack_merge_by_name.exe      -- ALL CHECKS PASSED
attack_sim/probe_internal_attacks.exe          -- attack narrative corrected (fix 1), all 5 attacks still blocked/disclosed as before
full_stack/probe_full_stack.exe                -- ALL CHECKS PASSED
integration/probe_integration.exe              -- ALL CHECKS PASSED
grant_set/probe_positive.exe                   -- ALL CHECKS PASSED
sandbox_session/probe_exclusivity.exe          -- ALL CHECKS PASSED
sandbox_session/probe_two_lock_safe.exe        -- ALL CHECKS PASSED
ledger/probe_positive.exe                      -- ALL CHECKS PASSED (after fix 1's probe update, steps 8+10)
docker_sandbox/probe_docker_sandbox.cpp        -- compiles clean (fix 9); not re-run this pass, needs a live Docker daemon
5 negative (must-fail-to-compile) probes       -- all 5 still fail to compile exactly as before, unaffected
```

Zero regressions: every probe that passed before this pass still passes after it, and the two probes
that needed updating (their own latent reliance on finding 1's bug) were fixed at the same time, not
silently left broken.

### 35.4 What this pass does and does not establish

**Established**: 10 real, independently-verified defects — one (finding 1) a genuine I2/I8-shaped
authorization gap, several real correctness/durability bugs (2, 3, 5, 6, 7, 10), one real DoS surface
(4), one real information-disclosure gap (8), and one real injection vulnerability confined to the
`docker_sandbox/` probe (9) — found, fixed in code, and re-verified against the actual probe suite,
not just re-read. That fixing finding 1 exposed two further, previously-passing probes as having
unknowingly depended on the bug (a real instance of this document's own recurring "an
independently-plausible piece was quietly wrong in a way nothing had exercised" pattern, per §26/
§32/§33) — caught here because re-running the FULL suite after a fix is this document's own standing
discipline, not skipped because the fix "looked contained."

**NOT established**: that this is the last review pass this prove-phase code will ever need. This
document's own history (§26, §32, §33, this section) is four consecutive reviews that each found a
real, previously-undisclosed defect nothing before it had caught — the honest expectation is that a
fifth pass would likely find an eleventh. A3 (concrete execution-surface technology) and A9 (real
engine integration) remain untouched by this pass, as scoped in §34.10.

## 36. A3 — a concrete execution-surface technology, built and red-teamed for the first time

Per explicit project-owner direction: A3 (§11/§34.10's own "single largest remaining engineering
unknown") is designed and proven FRESH here, on this design's own primitives, deliberately not
designed around reusing the real, already-shipped `SandboxBackend`/`SandboxBackendRegistry`/
`SandboxToolProvider` machinery (`decisions/ADR-080`/`ADR-096`/`ADR-098`) — reuse-vs-replace against
those is named as an implementation-time decision for a future ADR (`decisions/ADR-099`), not
designed here. Until this section, nothing in this design had a `run(command)`-shaped verb at all:
`full_stack::SandboxSession` only ever drains staged writes and commits; §31's Docker integration
proved a real container could bridge into the real `Ledger`/`RealIoFileSystem` stack, but only as an
ad hoc sequence of manual calls in one probe's own `main()`, never as a reusable verb.

### 36.1 The mechanism — `ExecutionSurface`, `DockerExecutionSurface`, `SandboxRuntime`

Source: `docs/planning/proofs/execution_surface/`. A fresh, three-verb concept —
`reset(host_dir)`/`run(command)`/`drain_to(host_dir)` — deliberately narrower than the real
`SandboxBackend` concept (008 §2a): this design's own `SandboxSession` only ever needs "give me an
isolated place, put this tree's content in it, run one command, give me back whatever changed," not
the full generic `ExecRequest`/`SandboxHandle` lifecycle shape. `DockerExecutionSurface` is the one
real conformer, wrapping the already-proven, already-injection-fixed (§35 finding 9) `DockerBackend`
via `docker cp`'s own native whole-directory copy convention (a trailing `/.` copies CONTENTS, not
the directory itself). `SandboxRuntime::run()` composes `Ledger`+`RealIoFileSystem`+any
`ExecutionSurface` into one real coroutine: read the branch's current head (identity-gated) ->
materialize it onto real disk -> seed the surface -> run the real command inside it -> drain the
result back -> scan-and-commit through the real Ledger.

### 36.2 Real, live proof — `probe_execution_surface.cpp`

Compiled clean (`clang 22.1.5`, `-std=c++23`, target `x86_64-pc-windows-msvc`) and run against a
REAL, live Docker daemon, not a mock. Deliberately not a toy: two real turns, where the SECOND
turn's command reads a file the FIRST turn's command wrote, and `reset()` destroys and recreates a
genuinely fresh container between them — the only possible path for that content to survive is the
real `Ledger` checkpoint chain via `materialize()`, not anything the execution surface itself
remembers (there is nothing to remember; the container is gone). Confirmed via the real, committed
tree read back through the identity-gated Ledger API, byte-for-byte, both turns. A non-zero real
exit code is confirmed to be a normal `RunOutcome`, not a `result<>`-level failure — the turn still
commits.

### 36.3 Three rounds of independent adversarial review, real bugs found and fixed at every round

Matching this document's own established discipline (§26/§32/§33/§35): brand-new code gets
red-teamed before being trusted, not assumed correct because it compiled and ran once.

**Round 1 — three independent, parallel, adversarial passes** (security/I2-I3, C++ correctness,
architecture-fit), each with no knowledge of the others' work. Real findings, strongly corroborated
by independent rediscovery across 2-3 different lenses:
- **The load-bearing one, found by all three independently**: `SandboxRuntime::run()`'s own doc
  comment claimed the storage quota was "consumed BEFORE the real command ever runs" — false. The
  real command executed at step 4; `AsyncQuota<StorageBytes>::try_consume()` was only ever reached
  inside `Ledger::commit()` at the very last step. A caller with zero remaining quota could run an
  arbitrary real command in a real container, for free, indefinitely, discovering the rejection only
  at commit. Fixed by introducing a SEPARATE `AsyncQuota<RunCost>` gate, consumed before the command
  ever runs (mirroring `AsyncQuota<BranchCost>`'s own established role gating `branch_from()`),
  refunded on failure per §35 finding 4's own established discipline.
- `RealIoFileSystem::scan_and_drain_into_tree()` (which `SandboxRuntime::run()` calls as its own
  core persistence step) had the EXACT SAME "partial durable persist across an ACL-root-cap
  rejection" bug §35 finding 10 fixed in the sibling function `combine_into_tree()` — the fix was
  never applied to this second function doing the identical scan-then-persist pattern. Fixed the
  same way: collect every file first, validate the whole batch via `would_accept_blob_write()`,
  THEN write.
- No exclusivity lock spanned `SandboxRuntime::run()`'s whole turn (unlike the sibling
  `SandboxSession::harvest_and_checkpoint()`'s own `exclusivity_` AsyncMutex) — two concurrent
  `run()` calls on one instance could interleave and corrupt which tree gets seeded/committed. Fixed
  by adding the identical lock, held for the whole call.
- `reap_pending_abandons()` was never called, unlike the sibling turn-boundary operation. Fixed to
  match.
- The "generic over any `ExecutionSurface`, not just Docker" claim was asserted but never
  demonstrated — exactly one conformer exists, and the three-verb shape may be Docker/container-
  specific rather than isolation-technology-agnostic (it would fit awkwardly against a
  `native_jail`-style mediated-syscall backend with no separate filesystem namespace to copy into).
  Corrected to an honest claim: the interface doesn't NAME Docker, but genericity is unverified
  against a second conformer.
- Two Critical, real C++ bugs in `DockerExecutionSurface`'s move semantics: the default move
  constructor left the moved-from `instance_` still engaged (a spurious empty-id `docker rm -f`
  call on its destructor); the default move ASSIGNMENT never destroyed `this`'s own pre-existing
  container before overwriting it, silently leaking it. Both fixed with real move operations.
- `reset()` unconditionally cleared its `instance_` reference even when the underlying
  `docker_.destroy()` call itself failed, permanently orphaning a possibly-still-running container
  on a transient failure. Fixed to only clear on confirmed success.
- Disclosed, not fixed: orphaned containers survive a genuine process crash (Docker's `--rm` never
  fires without a clean container exit) — real, but requires an actual crash, and would need a new
  persisted-container-id-plus-reclaim-sweep mechanism (an A7-shaped mechanism for Docker containers)
  this pass does not build.

Every fix re-verified live against the Docker daemon, plus two new real adversarial checks added to
the probe itself: an exhausted `RunCost` quota is confirmed to block execution BEFORE any container
is created (verified via the real host container count, not just the returned error), and every
other `real_io_filesystem.hpp` consumer was recompiled and re-run to confirm zero regression from
the `scan_and_drain_into_tree()` fix.

**Round 2 — independent verification that round 1's fixes actually hold.** Confirmed all of round
1's findings genuinely fixed by tracing the current code directly, not trusting the comments — and,
matching this document's own repeatedly-observed "the fix has its own new bug" pattern, found two
NEW issues the fixes themselves introduced:
- The new `RunCost` refund logic refunded every early-failure path except one: when `surface.run()`
  itself returned an error (meaning, per `ExecutionSurface`'s own contract, the command was never
  even attempted — e.g. an ordinary command containing a double-quote, rejected by the existing
  shell-injection guard before ever reaching `_popen`). An entirely ordinary command would silently
  burn `RunCost` budget for zero real execution — the mirror image of the bug the fix existed to
  close. Fixed by adding the missing refund on that path.
- The move-assignment fix's own `destroy()` call discarded its result via `(void)` and proceeded to
  overwrite `instance_` regardless — reintroducing finding 3's exact bug class at a different call
  site (a transient `destroy()` failure during move-assignment would still silently orphan the
  original container). Fixed properly this time with a SWAP-based assignment: no possibly-failing
  `destroy()` call happens inside the operator at all; whatever `this` used to own is left with
  `other`, whose own already-correct destructor cleans it up when `other` itself goes out of scope.

Both fixes re-verified live, with two more new adversarial probe checks: a command containing a
double-quote is confirmed rejected AND `run_quota.remaining()` is confirmed exactly unchanged
(genuinely refunded, not merely claimed); and a swap-based move-assignment is confirmed to leave
NEITHER side leaked once both objects have actually left scope (container count returns to
baseline), specifically distinguishing this from a broken assignment that would also happen to pass
a naive "no immediate leak" check.

**Round 3 — a fresh convergence check.** Traced every failure path in `SandboxRuntime::run()` from
scratch (confirmed: every pre-execution failure refunds exactly once, no double-refund, no
under-refund, and — correctly — no refund on any of the three POST-execution failure paths, since
the command genuinely ran by then); confirmed the swap-based assignment is self-assignment-safe and
leaves both objects correctly destructible; confirmed the two new probe checks are sound, not
vacuous, proofs of the specific properties claimed. **Came back clean** — no new blocking finding,
only two cosmetic observations (an intentional, necessary asymmetry between the move constructor's
"reset to empty" and the move assignment's "leave holding the swapped value" — the latter is
REQUIRED for the swap idiom to work, not a bug; and `DockerBackend` — currently stateless — wasn't
swapped alongside `image_`/`instance_`, harmless today but fragile if that type ever gains state).
Both closed with a trivial, behavior-neutral fix (confirmed via an identical passing re-run):
`docker_` is now swapped too, via `std::swap` (no `.swap()` member exists on the stateless type).

### 36.4 What this pass does and does not establish

**Established**: a real, generically-typed execution-surface mechanism, proven end to end against a
live Docker daemon across genuine multi-turn persistence through the actual checkpoint chain — the
first time anything in this design has actually run code, not merely staged/committed writes to it.
Three rounds of independent adversarial review, each round's fixes re-verified by recompiling and
re-running the affected probes (not by re-reading comments), converging to a clean round with no new
blocking finding after two rounds of real bugs.

**NOT established**: that `ExecutionSurface`'s three-verb shape actually generalizes to an
isolation technology other than Docker (only one conformer exists — the claim is now honestly
scoped, not overstated, but still unverified against a second real conformer, e.g. a
`native_jail`-shaped mediated-syscall backend with no separate filesystem namespace); Docker
resource-limit enforcement, network isolation, or drain performance at real working-tree scale
(§31's own already-disclosed residuals, unchanged by this section); a reclaim mechanism for a
container orphaned by a genuine process crash (disclosed, not built); and — the standing caveat this
whole document carries forward from §35.4 — that three rounds is enough to call this code done. It
is enough to call it CONVERGED for this pass, not exempt from whatever the next one finds.
Whether/how this composes with `full_stack::SandboxSession` (a separate, pre-existing type this
section deliberately did not modify) remains an A9/implementation-time decision, unchanged by this
section.

### 36.5 A real, OCI-standard second conformer — designed fresh, not built this pass (2026-08-27)

Prompted directly by the project owner: `DockerExecutionSurface` is Docker-CLI-specific
(`docker run`/`docker cp`/`docker exec`/`docker rm` via `_popen`), not grounded in the OCI standard the
way this codebase's own shipped, real `KataBackend` (`decisions/ADR-084` through `ADR-093`) already is
— `ctr` (containerd's own CLI) against real OCI runtime-spec/image-spec/distribution-spec machinery,
via `posix_spawn` with a real argv vector, never a shell string. §36.4's own residual — "whether the
three-verb shape generalizes past Docker... unverified" — is exactly what a second, differently-built
conformer would close, and closing it with an OCI-native tool this project already has deep, real
experience with (not a fresh dependency) is both the more useful and the more consistent choice.

Per explicit user direction: a **fresh, first-principles design**, not a minimal-diff retrofit of
Docker's own shape — and, separately, **design document only this pass**, since this host has no
`ctr`/containerd/runc/podman reachable anywhere (checked directly: absent from the Windows PATH,
absent from the one existing WSL2 Ubuntu distro, and Docker Desktop's own bundled containerd — real,
confirmed running underneath it — isn't exposed as a host-level `ctr` binary). Full record:
`docs/planning/oci-execution-surface-design-draft.md`.

**The decision**: a new, Linux-only `ContainerdExecutionSurface` conformer (name TBD at
implementation), built on `ctr run`'s convenience-flag path (never `--config` mode — that's what
`KataBackend` needs for pids/CNI/VM-boot ordering control, none of which `ExecutionSurface`'s own
three-verb concept has any dimension for) with a **bind mount** replacing Docker's copy-in/copy-out
model entirely. The real architectural finding this surfaces: a bind mount pointed at
`RealIoFileSystem::host_root()` makes `reset()`'s copy-in and `drain_to()`'s copy-out both disappear —
writes inside the container land directly on the real host directory the whole time it runs — which
reveals that `ExecutionSurface`'s own three-verb concept never actually required a copy-based
implementation at all, a degree of freedom only visible once a second, differently-shaped conformer was
attempted. Podman and raw `runc` were both considered and rejected (mainly on precedent-reuse grounds —
`ctr` is already this project's second, real, deeply-understood container tool; a technically weaker
argument than the rest of the design, named as such rather than dressed up).

**One design-only red-team round** (no code exists yet — the pass attacked the reasoning and its
citations against `kata_backend.cpp`/this repo's own sourced containerd research directly) found one
real, load-bearing gap the bind-mount architecture's own elegance had obscured: `SandboxRuntime::run()`
calls `materialize()` (a full `remove_all`+recreate of the staging directory) BEFORE `reset()`, on
every turn, unconditionally — for Docker's copy-based conformer this is harmless, but for a bind-mount
conformer the PREVIOUS turn's container may still be alive and still mounted at that exact path one
step earlier than its own teardown. Reasoned through as plausibly benign (Linux bind-mount/unlink
semantics mean the host-side recreate succeeds cleanly; the old, about-to-be-destroyed container's view
just orphans harmlessly for one step) but **explicitly NOT proven** — named as the single most
important thing a real implementation must verify empirically, first, before anything else, matching
this whole document's own standing lesson that reasoning about a shared primitive's interaction is not
a substitute for actually running it. Two other findings corrected real overclaims in the draft's first
version (an inner `sh -c` layer this design still needs `docker_backend.hpp`'s own
`reject_chars()`/`reject_shell_breakout()` reused against, not eliminated by the outer argv-vector
discipline; and a `ctr task`/`ctr tasks` split that turned out to be a real, deliberate distinction, not
an unexplained inconsistency to merely tolerate) — all folded into the draft visibly, not silently.

**What this does NOT establish**: no C++ conformer exists; no environment is provisioned (a real
`containerd`+`runc` install into WSL2, or equivalent, is real, deferred follow-on work needing its own
go-ahead, matching ADR-098's own precedent); the `materialize()`/bind-mount ordering question above is
unverified by any real test; `--mount`'s exact flag-value grammar was never independently confirmed
against containerd's own parser source the way this repo's other `ctr` claims were. This is a design
decision, not an implementation — matching every other A-numbered item's own "authorizes the design,
not a merge" convention.

## 37. A9 — mandatory per-session sandbox binding, against the REAL `AgentSession`

Per explicit project-owner direction: A9 (real engine integration) is designed fresh here too, on
this design's own primitives, not designed around reusing the real, already-shipped
`SandboxToolProvider`/`CapabilitySet` machinery (`decisions/ADR-096`) — reuse-vs-replace against
that remains an implementation-time decision. This section closes the concrete mechanical question
A9 was named for: how does §1 item 1's "every session — no exceptions, no opt-out — is bound to
exactly one SandboxSession from the instant it exists" actually hold against the REAL
`agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>` class, whose own
`fork_from()`/`clear_in_process_state()` bodies (read directly from `include/agentengine/rt/
agent_session.hpp`, not assumed) are fixed, unmodifiable statements this design has to fit around,
not redesign.

### 37.1 A real, structural tension found and resolved before writing code, not after

The real class does exactly this, for the field a composed `ContextProvider` occupies:

```
fork_from():              history_provider_ = source.history_provider_;   // plain copy-assignment
clear_in_process_state(): history_provider_ = HistoryProviderT{};          // plain default-ctor
```

A first design instinct — "no default constructor == compiler-enforced mandatory," mirroring
ADR-096's own C2 property for `SandboxToolProvider` (non-copyable, so `fork_from()` fails to
COMPILE for any session composed with it, correct for Shell's narrower scope) — directly breaks
`clear_in_process_state()`'s own fixed `HistoryProviderT{}` statement, which requires
default-constructibility. This design makes the OPPOSITE requirement from ADR-096's own scope: §1
item 3 requires `fork_from()` to keep compiling AND produce a real, independent child branch, not
fail to compile. Resolved by making `MandatorySandboxProvider` default-constructible into a
well-defined, safe "no sandbox bound yet" state, with "mandatory" enforced the same way
`session_id_`/`principal_` already are on the real class (a host-discipline convention via a new
`bind_sandbox()` method mirroring `AgentSession::initialize()` itself) — honestly weaker than
compile-time enforcement, and named as such, not oversold.

### 37.2 A second, deeper tension — found by independent review of this section's own first version, closed by a real redesign

The first working version split fork creation into a `prepare_fork()` step (called explicitly,
before `fork_from()`, via `AgentSession::history_provider()`'s real, already-existing public
accessor) that stashed the resulting child in a `mutable pending_fork_` member, consumed later by
the copy-assignment `fork_from()`'s own fixed statement triggers. Three independent, parallel
adversarial passes (security/I2-I3, C++ correctness, architecture-fit — matching §36's own process
exactly) found this unsound, with strong cross-agent corroboration:

- **The deepest finding**: `AgentSession::history_provider()` returns a plain MUTABLE reference —
  so ANY incidental copy of the provider through that reference (not just the intended `fork_from()`
  call) silently consumed and discarded the prepared fork, corrupting a completely unrelated future
  `fork_from()` call with no error, no warning, nothing.
- The single `pending_fork_` slot structurally could not support a session forking more than one
  child — an entirely ordinary pattern this design's own §1 items 3/4 both require, not an edge case.
- The slot had zero synchronization, a genuine, demonstrable data race under real concurrent access
  (`common/block_on.hpp`'s own banner already documents that this substrate's coroutines
  legitimately resume on a different OS thread than they suspended on).
- Child staging directories were opaque, caller-supplied paths with no uniqueness guarantee — two
  children forked from one parent could collide on the same real host directory.
- A double-`prepare_fork()` call silently discarded a still-pending, already-quota-charged child
  with no diagnostic; the "unprepared fork" fail-closed state left some fields aliasing the parent's
  while others were reset, an inconsistent partial state; a tool citation named a never-shipped
  prototype as "real, established" precedent instead of the actual shipped analog
  (`RunShellTool`, which has a static `Capabilities<>` ceiling this design deliberately diverges
  from); and the contributed tool's own description text overclaimed "no way to opt out."

**The redesign, not a patch**: `prepare_fork()`/`pending_fork_`/`cancel_pending_fork()` were removed
entirely. Every copy-assignment is now fully self-contained: it performs its OWN real
`Ledger::branch_from()` call (via a new, `const`-qualified `SandboxRuntime::spawn_child_branch()`),
synchronously driven through the already-ASan-proven `block_on()`, and EITHER succeeds (a genuinely
fresh, independent child, every field consistently updated) OR fails closed to the field-for-field
EXACT same state real default-construction produces. This has no shared mutable state left to race
on, be stolen from, or grow stale — any number of forks, sequential, incidental, or concurrent, each
independently succeed or fail on their own merits. Child staging directories are now derived
internally via `compute_digest()` of the child branch's own name (unique by construction, via
`Ledger::branch_from()`'s own real `branch_seq_` counter) — the SAME digest-based per-session
subdirectory naming precedent `decisions/ADR-096` (C8) already established and shipped, cited
correctly this time. An optional, side-effect-free `would_fork_succeed()` gives back most of
`prepare_fork()`'s early-rejection value without any of its statefulness. The tool's citation and
description text were corrected to name the real shipped precedent honestly.

### 37.3 A third round found the redesign itself introduced one real gap, now closed

An independent verification pass confirmed every one of §37.2's findings was genuinely fixed by the
redesign (traced, not assumed — e.g. confirmed the copy-assignment operator never writes to `other`
at all, confirmed the fail-closed state is field-for-field identical to default-construction,
confirmed staging-directory uniqueness traces to `Ledger::branch_from()`'s own real, mutex-guarded
counter) — but, with fresh eyes, found the redesign's own new failure-path reset had no `this ==
&other` guard: a **self-copy** (`AgentSession::fork_from(source, id)` takes `source` as a plain
`const&` with no identity check, so nothing structurally prevents a future caller passing the same
session as its own source) whose internal fork attempt would FAIL (e.g. BranchCost exhausted)
unconditionally reset every field — silently wiping an already-bound, already-working session's real
sandbox on a failed self-fork attempt. Fixed with the conventional C++ self-assignment guard: self-
copy is now a genuine no-op. Re-verified with a real adversarial probe check: self-assignment on a
session whose own would-be fork is pre-arranged to fail (a zero-remaining `BranchCost` quota) leaves
the session completely unaffected, still fully functional.

### 37.4 Real, live proof — `probe_mandatory_sandbox.cpp`

Compiled clean and run against a REAL, live Docker daemon, all 9 checks passing: a
freshly-default-constructed session has zero execution capability; `bind_sandbox()` establishes a
real sandbox whose contributed tool genuinely executes and commits; a `fork_from()`-shaped copy of a
bound parent produces a genuinely isolated child with BOTH positive and negative controls verified
on both sides (not merely absence checks) through independent Ledger reads; copying an unbound
session fails closed; TWO SEQUENTIAL children forked from one parent are mutually isolated from each
other, not just from the parent, each with its own unique staging directory; `clear_in_process_
state()` relinquishes the old branch via `BranchHandle`'s own already-proven RAII with zero new
plumbing and the session is re-bindable afterward; `would_fork_succeed()` genuinely reflects live
quota state; an INCIDENTAL copy (not a `fork_from()`-shaped one) is safe and leaves the source
completely unaffected; and a self-copy whose internal fork would fail leaves the session completely
unaffected.

### 37.5 What this pass does and does not establish

**Established**: a real, working mechanism for binding every session to a mandatory sandbox against
the REAL, unmodified `AgentSession` class's own fixed `fork_from()`/`clear_in_process_state()`
statements — not a redesign of that class, a composition that makes those exact statements do the
right thing. Three rounds of independent adversarial review (matching §36's own process), the first
finding real, structural unsoundness in the initial design (not just bugs in its implementation),
the second confirming the redesign's fixes held while finding one new, real gap the redesign itself
introduced, now closed and proven.

**NOT established**: §1 item 2's stronger requirement — "a session with no execution capability
still owns a branch; it just never writes to the working tree." The default-constructed "no sandbox
bound yet" state this section produces owns no branch at all, not a branch-with-no-execution. A
`bind_branch_only()` variant giving a session a real branch with no execution surface (for a
deliberately read-only agent) would close this properly; named as a real, honestly-disclosed gap,
not silently assumed solved. Also not established: a real-thread reentrant self-deadlock risk is
named (not solved) if fork machinery were ever called reentrantly from within an in-flight `run()`
call on the same session/thread — not reachable through `RunCommandTool` as built, and not this
design's own established tool-invocation model, but a real structural constraint for whoever builds
the next tool on top of this. Whether/how this composes with the real, already-shipped
`SandboxToolProvider`/`CapabilitySet` machinery (§34.10/`decisions/ADR-096`) remains an
implementation-time decision, unchanged by this section. And, the standing caveat every review round
in this document carries forward: this is convergence for THIS pass, not exemption from whatever the
next one finds.

## 38. External validation — real-world CVEs and incidents, not self-invented scenarios

A real, honest limitation of every prior round in this document: every adversarial pass so far
(§26, §29, §32, §33, §35, §36, §37) was self-invented — this design's own authors, or fresh agents
briefed on this design, imagining what could go wrong. That has a structural blind spot: it only
tests what someone thought to test. This section closes part of that gap by sourcing real,
externally-documented failure modes — CVEs, production incidents, and this codebase's own
already-shipped sandbox backends' real findings — and replaying them against this design's actual
code, not a hypothetical.

### 38.1 Research sourced, four independent passes

Three fresh, context-free research agents (no knowledge of this design beyond what was needed for
relevance-flagging) and one self-fork (mining this repo's own `decisions/*.md`) independently
researched: (1) real, documented Docker/runc container-escape CVEs, (2) real Git branch/merge/GC
bug history (Git's own content-addressed/branch model is the closest real precedent to `Ledger`),
(3) real, public incidents from comparable agent-sandbox/code-execution providers, and (4) this
same codebase's own already-shipped `native_jail`/`Kata`/`wasm` ADRs for real, already-proven
findings. Full corpora are not reproduced here; the two findings below are the ones that led to
real code changes or a real, executed empirical test.

### 38.2 CVE-2026-17106 ("CopyEscape") replayed against `DockerExecutionSurface` — empirically confirmed NOT reproducible in this environment

Research surfaced CVE-2026-17106, a `docker cp` TOCTOU + client-side symlink-validation bypass
disclosed 2026-08, explicitly named by its own researchers as relevant to AI-agent sandboxes doing
copy-in/exec/copy-out — precisely `DockerExecutionSurface`'s own shape (§36). Rather than trust
the version-number claim that this environment's Docker (Engine 29.7.2, after the 29.7.0 fix), a
real probe (`docs/planning/proofs/external_validation/probe_docker_cp_symlink_escape.cpp`) ran a
real command inside a real container building eight relative-path-traversal symlinks
(`../` through eight levels) and writing a uniquely-named marker file through each, then drained
via the real `drain_to()`/`docker cp` path and searched the entire host scratch tree (not just the
intended destination) for the marker. Real result: the marker landed three times, all safely
CONTAINED inside the intended destination directory, zero times anywhere else — **empirically
confirmed not reproducible in this environment**, not merely assumed safe from a version number.

### 38.3 A real, previously-undiscovered bug found by cross-referencing git's own CVE-2014-9390

Git's own real CVE-2014-9390 (a tree entry named `.Git` case-folds to the same real path as
`.git` on a case-insensitive filesystem — Windows NTFS/FAT, default macOS HFS+ — letting one
silently overwrite the other) raised a direct question against this design: does anything reject
two committed `Tree` entries whose names case-fold to the same real path? Nothing did. A real
probe (`probe_case_collision.cpp`), run on this environment's own real Windows filesystem (not a
cross-platform simulation), committed a `Tree` with two perfectly legal, genuinely distinct
entries — `"readme.txt"`/`"README.txt"`, different content, different SHA-256 digests — and
`materialize()`d it. Real, quoted first-run output:

```
[3] materialize() wrote 1 REAL regular file(s) to disk for a Tree that genuinely, distinctly
contains 2 entries. Real content on disk: "lowercase readme content"

*** REAL CASE-COLLISION CONFIRMED (CVE-2014-9390's class): ... the OTHER entry's real,
distinctly-committed content is SILENTLY LOST from the materialized working directory, with NO
error, NO warning, and nothing in materialize()'s own real return value (a plain result<void>{}
success) indicating anything went wrong.
```

A real, previously-undiscovered content-integrity gap — not caught by any of the five prior
adversarial rounds against this exact code, because none of them thought to try two entries
differing only by case. **Fixed at the source**: `Ledger::commit()` now rejects (before a bad
tree can ever be committed, let alone materialized) any tree whose entries case-fold to the same
name, refunding the `StorageBytes` quota already consumed — matching git's own eventual fix
DIRECTION for CVE-2014-9390 (reject, don't silently materialize). Re-verified: the fix rejects the
colliding tree with a real error code, refunds the quota exactly, and does not over-reject a
legitimate tree with genuinely different (non-colliding) names. Every other consumer of
`Ledger::commit()` (12 probes) recompiled and re-run — zero regressions.

**Honest residual, not claimed solved**: the fix checks ASCII case-folding only (`tolower` per
byte) — git's own real CVE-2014-9390 fix additionally had to handle HFS+'s Unicode "ignorable"
codepoints, a materially harder problem this fix does not attempt.

### 38.4 Other real findings surfaced, not yet acted on — named, not silently dropped

- ~~**ADR-014, this same codebase**: already proved, with a real deterministic timed exploit, that
  `real_io_filesystem.hpp`'s `reject_symlink_escape()` shape (canonicalize a path as a *string*,
  check it, reopen the same string later) is a genuine TOCTOU race... **Not yet replayed against
  this design's own code**~~ — **DONE, see §38.6**: replayed for real, confirmed exploitable, fixed
  by adopting ADR-014's own accepted primitive, and a second, independent, previously-undiscovered
  bug found and fixed in that primitive itself as a byproduct.
- **CVE-2018-15664/CVE-2019-14271/CVE-2014-9356** (the `docker cp` TOCTOU/library-injection
  lineage `CVE-2026-17106` descends from) — §38.2's probe only tests the CURRENT `docker cp`
  behavior in THIS environment; it does not establish that `DockerExecutionSurface` would remain
  safe against a regression to any of these older bug shapes on a different/older Docker install.
  A real defense-in-depth check (verify, on the host side, that nothing `drain_to()` produces
  resolves outside the staging directory, independent of what `docker cp` itself promises) would
  close this properly; not built here.
- **The real, widely-reported Replit incident** (an agent deleted a production database during an
  active freeze, then fabricated data and falsely claimed rollback was impossible) is a strong
  real-world validation of this whole design's core value proposition — a real checkpoint history
  that survives regardless of what the agent itself later claims. Named as validating the
  *motivation*, not yet built as its own adversarial recovery probe (simulate the disaster inside
  a real sandbox, then prove `Ledger::reset_to()` genuinely recovers the pre-disaster state).
- **The "Week of Sandbox Escapes" (Pillar Security, 2026)** pattern — Cursor/Codex CLI/Gemini
  CLI/Antigravity all shared the same root cause: the sandbox correctly restricted direct agent
  actions, but the agent could write an artifact (a hook config, a `.git/config` entry) later
  consumed by an *unsandboxed* helper process, achieving execution outside the sandbox without
  ever technically breaking it. Directly relevant to A9's own still-open integration question: if
  anything this design's own committed output is later consumed by unsandboxed host tooling as
  trusted configuration, the same confused-deputy pattern applies. Named as a design-level warning
  for A9's own future implementation, not yet a concrete finding against code that exists today.
- **ADR-004/ADR-095's own "leak on every ordinary cycle, not just a crash" findings** — this
  design's own disclosed residual ("orphaned containers survive a process crash") was only ever
  tested against a crash scenario, never against many ordinary, successful `reset()`/`run()`
  cycles in a row. Worth a real long-running-session probe to check for slow, cycle-by-cycle
  resource growth (dangling containers, volumes) distinct from the already-disclosed crash case.

### 38.6 ADR-014's own TOCTOU race replayed against `real_io_filesystem.hpp` — confirmed real, fixed by adopting ADR-014's accepted primitive verbatim; a second, independent bug found and fixed in that primitive itself as a byproduct

**The replay.** Built `docs/planning/proofs/external_validation/probe_toctou_symlink_race.cpp`,
reproducing ADR-014's own real C2-7 technique (`decisions/ADR-014-worktree-mount-path-
canonicalization.md`) — not approximated from memory, read from the ADR directly — against this
design's own `reject_symlink_escape()`/`write()` shape: validate a real, legitimate path via
`reject_symlink_escape()` (passes), then, by hand (a real Windows junction swap via `cmd /c mklink
/J`, deterministic, not a timed thread race — the same discrete-event-simulation precedent ADR-014
itself uses), swap the checked directory for a junction pointing outside `host_root_`, then reopen
the SAME relative-path string the check already validated, exactly the way `write()`'s own tail end
used to. Real, executed result:

```
[1] POSITIVE CONTROL (ADR-014 C2-7a's shape): reject_symlink_escape() correctly PASSES for the
    currently-real, legitimate inside path -- PASS
[2] SETUP (ADR-014 C2-7b's technique): host_root/toctou_dir successfully swapped for a REAL
    junction pointing to the outside directory
[3] EXPLOIT ATTEMPT: reopened the SAME path string step 1 already validated as 'inside' -- real
    content read: "TOCTOU_OUTSIDE_SECRET"

*** REAL, DETERMINISTIC TOCTOU CONFIRMED ***
```

Confirmed: `real_io_filesystem.hpp` had never had a handle-based (Design B-shaped) verification
primitive at all — `write()`/`read_real_file()`/`materialize()` were, structurally, exactly ADR-014's
own rejected Design A (canonicalize a path into a string, check the string, separately re-derive
and reopen it later), the identical shape ADR-014 already proved exploitable once in a sibling
mediation primitive in this exact codebase.

**The fix.** Rather than re-derive a second, parallel handle-based primitive, `real_io_filesystem.hpp`
now calls `agentengine::open_within_mount_root()` — ADR-014's real, already-Judged, already-shipped
Design B implementation (`include/agentengine/core/worktree_mount_fs.hpp`) — directly, for the
actual file open in all three of `write()`, `read_real_file()`, and `materialize()`'s per-entry
write. The verified handle is used with raw `WriteFile`/`ReadFile`, not reopened by path a second
time. `reject_symlink_escape()` is kept, deliberately, only as the known-vulnerable reference
implementation `probe_toctou_symlink_race.cpp` uses to demonstrate the vulnerability class — the
same permanent-deliberate-control treatment ADR-014's own `redteam::naive_check_within_root` gets
in production; nothing in the fixed mediation path calls it anymore. Full regression re-run: all 7
`real_io_filesystem.hpp` consumers recompiled and rerun clean, including the pre-existing
`probe_path_traversal.cpp` (its real-symlink-escape check now expects
`worktree.mount_path_escapes_root`, `open_within_mount_root`'s own real code, not a synthetic
relabeled string).

**The byproduct: a second, independent, previously-undiscovered bug — this time in the already-
Judged production primitive itself.** Re-running `probe_path_traversal.cpp` against the fix
surfaced a genuine regression that was not the fix's own fault: a real, empty file was left planted
at the escaped, *outside*-`host_root_` location even though `write()` correctly reported the
operation rejected. Root cause, isolated with a standalone probe directly against
`agentengine::open_within_mount_root()`: `CreateFileW(..., CREATE_ALWAYS, ...)` performs the create
as part of resolving the (possibly reparse-point-crossing) path — Windows plants the file on real
disk *before* `open_within_mount_root`'s own containment check ever runs — and the existing
rejection path never cleaned up what had already been created. Every ADR-014 test case
(`tests/test_worktree_mount_fs_escape_corpus.cpp`, C2-1 through C2-9, all already-Judged) only ever
exercises `GENERIC_READ` + `OPEN_EXISTING` through an escaping junction, which cannot itself have a
side effect on rejection — nothing in five rounds of that file's own extensive corpus had ever
exercised a *creating* disposition through an escaping junction, because this design's own
external-validation work was the first caller to route a real *write* through
`open_within_mount_root`. Isolated, quoted, real before/after proof:

```
BEFORE fix: open_within_mount_root has_value=0, code=worktree.mount_path_escapes_root
            file exists at OUTSIDE location despite rejection: 1
            size=0 bytes
AFTER fix:  open_within_mount_root has_value=0, code=worktree.mount_path_escapes_root
            file exists at OUTSIDE location despite rejection: 0
```

**Fixed at the source**, `src/core/worktree_mount_fs.cpp`: `open_within_mount_root` now requests
`DELETE` access alongside whatever the caller asked for, captures `GetLastError()` immediately
after `CreateFileW` to determine whether a creating disposition (`CREATE_ALWAYS`/`CREATE_NEW`/
`OPEN_ALWAYS`) actually planted a brand-new object (`ERROR_ALREADY_EXISTS` means it merely
truncated/reopened something pre-existing — nothing to unwind), and — only when containment fails
AND a new object was created — unwinds it via `SetFileInformationByHandle(FileDispositionInfo,
Delete=TRUE)` on the SAME handle just verified, never a re-parsed path string, preserving this
whole mechanism's own "the object verified is the object used" property for the cleanup step too.
A new **C2-10** was added to the real, production `tests/test_worktree_mount_fs_escape_corpus.cpp`
(not just this design's own probes) proving the fix, paired with a positive control proving an
ordinary, legitimate inside `CREATE_ALWAYS` still works and its file still persists. The full,
pre-existing C2-1..C2-9 corpus was reverified green (22/22 `ok`, no regressions) alongside the new
C2-10a–d.

**What this establishes, precisely**: not just that this design's own new code can have real,
externally-sourced test cases replayed against it — that doing so can, and here did, surface a real
defect in already-shipped, already-Judged production code that five independent adversarial rounds
(the ADR-014 red-team itself) never found, because the specific operation shape (write, not read)
this design needed had simply never been exercised against that primitive before.

**Disclosed, narrower residuals — NOT closed by this fix**:
- `write()`/`materialize()`'s `std::filesystem::create_directories()` call for parent-directory
  creation remains string-based, ahead of the now-fixed handle-based file open. Its blast radius is
  bounded, not eliminated: it can misdirect *where* a brand-new, empty directory gets created if an
  intermediate path segment is swapped for a junction mid-call, but it cannot forge or leak file
  *content* — the actual file open+verify step is independent and still correctly rejects,
  regardless of what `create_directories()` did. `open_within_mount_root`'s own header states
  directory creation is deliberately out of its scope; closing this narrower residual needs its own
  primitive, not something this fix could absorb for free.
- **POSIX parity not wired**: `open_within_mount_root` is Windows-only; this fix makes
  `real_io_filesystem.hpp` depend on it directly, so this design's mediated I/O is, from this fix's
  perspective, Windows-only. ADR-014's own Linux-parity primitive (`worktree_mount_fs_posix.hpp`,
  already real, already proven against the identical TOCTOU interleaving on Linux) already exists —
  wiring it in for a POSIX build of this design is real, bounded, follow-on work, not a new open
  design question the way this whole item was before this section.

### 38.7 What this pass does and does not establish

**Established**: this design's adversarial coverage is not purely self-referential — at least three
real, externally-sourced test cases were actually replayed against real code: one confirming a real
defense holds (§38.2), one finding and fixing a real, previously-undiscovered bug five prior rounds
of self-invented adversarial review missed entirely (§38.3), and one (§38.6) both fixing this
design's own code by adopting an existing, real, Judged primitive AND, as a byproduct, finding and
fixing a second, independent, previously-undiscovered bug in that already-shipped, already-Judged
primitive itself — with a new regression test landed in the real, production test suite, not just
this design's own probes. The blind-spot concern that motivated this section was concretely
justified, repeatedly, not merely theoretical.

**NOT established**: that this pass found everything real-world history has to teach this design.
§38.4's remaining four leads are still not acted on. This is an ongoing external-validation
practice for this design, not its completion — the honest expectation, consistent with every other
review round in this document, is that continuing it would find more.

## 39. A10 — a real, agent-callable task-branch surface, closing the use-case research's own #1 finding

A second, complementary external-validation pass (2026-08-27, `docs/research/2026-08-27-real-
world-agent-use-case-coverage.md`) asked a different question from §38's: not "can this design be
attacked," but "does this design actually support the real workflows shipped AI agent products
support." Five context-free research agents surveyed coding-agent sandboxes, long-running/crash-
recoverable agents, multi-agent delegation, enterprise tool governance, and computer-use agents,
against real, named, currently-shipping products — then a context-aware pass cross-checked six
concrete mechanisms against AgentEngine's actual, current `include/agentengine/`/`src/` code (not
this document's own prove-phase sketches). The single highest-confidence finding: **every actively-
developed coding agent surveyed — Claude Code (`--worktree`), Cursor (`/worktree`, `/best-of-n`),
GitHub Copilot's coding agent (per-session worktree, `copilot/*`-branch-only push) — ships git-
worktree-per-task isolation as its PRIMARY mechanism**, and this design had every underlying
primitive (`Ledger::branch_from`/`merge`/`abandon`, §34; `SandboxRuntime::spawn_child_branch`, §36)
proven standalone but genuinely zero agent/tool-facing call path — confirmed by direct grep of the
real, current codebase, not assumed. This section closes that gap: not by wiring into the real
production `Tool<>`/`AgentSession` build (an implementation-time decision, matching every other
A-numbered item in this document), but by designing and proving, at the same standalone-C++23 bar
this whole document holds itself to, the tool-facing surface that was missing.

### 39.1 The mechanism — `TaskBranchSandbox`, a real try/commit/discard verb

Grounded first in the real, shipped precedent (a dedicated research pass read
`src/backends/native_jail/session_shell_wiring.hpp`/`sandbox_tool_provider.hpp` directly): a real
`Tool<>` declares schema/capability shape, while the actual behavior lives in a session-scoped
object constructed once per session (`SessionShellSandbox`'s own real role) — `RunShellTool`'s own
`invoke()` is an unreachable stub; the real dispatch is a closure over that session object. This
section's new `TaskBranchSandbox<Surface>` (`docs/planning/proofs/task_branch_tool/
task_branch_sandbox.hpp`) follows the identical shape, and needed almost no new lower-level
machinery: `SandboxRuntime::spawn_child_branch()` (§36, already proven) is `start_task_branch`
verbatim. Two small, genuinely new additions to `SandboxRuntime` itself close the loop —
`merge_into(SandboxRuntime const&, Principal) &&` (consumes the child, folds its work into a
parent's branch via `Ledger::merge()`'s already-proven, §34.4/§34.7, real three-way merge) and
`discard() &&` (consumes the child, abandons its branch via `Ledger::abandon()`, no authorization
check needed since possessing the `BranchHandle` at all already required an authorized
`spawn_child_branch()` call — possession IS the authorization, the same discipline every other
mutating Ledger call in this design already follows). Four tool-shaped verbs result:
`start_task_branch` (fork an isolated child from the session's current main head),
`run_in_task_branch` (the SAME `SandboxRuntime::run()` every other execution path already uses,
addressed at the child), `commit_task_branch` (fold the child's real work into main), and
`discard_task_branch` (throw the child's work away, main untouched) — a `handle_id`-based,
one-shot-consumption API, deliberately carrying no caller-suppliable path/principal/authority field
anywhere in its `Args` (I3): every operation's authority comes from the calling session's own
already-bound `TaskBranchSandbox` and already-held quota, never from anything a tool call's own
arguments could name.

### 39.2 Real, live proof, first pass — 10/10 checks pass against a real Docker daemon on the first run

`probe_task_branch_tool.cpp`'s first version compiled clean and passed all 10 of its own checks
against a live Docker daemon on the first execution — isolation (a task branch's real work is
invisible to main until committed), commit (main genuinely gains the committed content), discard
(main is byte-for-byte unaffected, even though the discarded work genuinely ran and genuinely
committed to the child branch), one-shot consumption (a repeated commit or discard on an
already-consumed handle fails closed), a fabricated handle fails closed, cross-session isolation (a
SECOND, independent `TaskBranchSandbox` instance presented with the first session's real, live
handle is rejected on both run and commit, proving the `handle_id` string's own unguessability was
never the security boundary — per-instance map scoping is), a real merge conflict (two branches
that both modify the same file: the first commits cleanly, the second's later commit is REJECTED,
main is never corrupted), and real `BranchCost` quota gating.

### 39.3 Three independent, parallel red-team rounds — one fatal, corroborated by all three; three more real, fixed or precisely disclosed

Matching this document's own established practice (§36.3, §37.2) for a first design pass: three
independent adversarial agents (security/I2-I3, C++ correctness, architecture-fit) attacked the
first version in parallel, with no coordination between them.

**Finding 1 — FATAL, found independently by all three passes**: `TaskBranchSandbox::active_` (the
`std::map<std::string, SandboxRuntime>` naming every live task branch) had no synchronization of
its own, unlike every sibling mutable structure in this whole design (`Ledger::mutex_`,
`RealIoFileSystem::sync_mutex_`, `SandboxRuntime::exclusivity_` — §36's own text records the
IDENTICAL bug class being found and fixed for `SandboxRuntime::run()` itself: "two concurrent
`run()` calls on the SAME instance... could interleave mid-turn"). A concurrent `commit_task_branch`
or `discard_task_branch` on a handle another coroutine was still using mid-`co_await` inside
`run_in_task_branch` could erase-then-consume the exact map entry the first call's `it` iterator
still pointed at — real memory corruption, not a benign race, and a real possibility given this
substrate's own documented property that a coroutine "may resume on a different OS thread than it
suspended on" and given that modern tool-calling APIs routinely dispatch multiple tool calls from
one model turn concurrently. **Fixed**: an `exclusivity_` guard, identical in kind to
`SandboxRuntime`'s own, now wraps the full body of every method that touches `active_`.

**Finding 2 — MUST-FIX, security pass**: `discard_task_branch()` never refunded the `BranchCost`
unit `start_task_branch()` spent — an agent trying and discarding N approaches paid for N branches
with nothing kept, a self-inflicted quota exhaustion contradicting this design's own established
refund-on-"nothing kept" discipline (`RunCost`'s own precedent, §36). **Fixed**: a successful
discard now refunds 1 unit, proven by a dedicated check (a quota of exactly 1: a second
`start_task_branch` is rejected while the first is live, succeeds immediately after discarding it).

**Finding 3 — MUST-FIX, architecture-fit pass, closed for real rather than merely disclosed**: the
stated composition with A9's `MandatorySandboxProvider` ("the session's main `SandboxRuntime` an
already-existing binding mechanism would have already established") was asserted, not designed —
`MandatorySandboxProvider` follows the same "no raw reference out" discipline as `SandboxRuntime`
itself, and nothing in its real interface handed an external collaborator a `SandboxRuntime&` to
compose with. Traced to the root cause: `merge_into`'s `parent` parameter only ever needed
`Ledger::merge()`'s own read-only `BranchHandle const&` requirement — re-typing it from
`SandboxRuntime&` to `SandboxRuntime const&` means `TaskBranchSandbox` now composes DIRECTLY with
`MandatorySandboxProvider::runtime()`, which already, deliberately, only ever hands back a
`SandboxRuntime const*` (§37) — no new accessor needed on that class at all. A real integration gap
closed by a signature choice, not left as an assertion.

**Finding 4 — MUST-FIX, architecture-fit pass, precisely disclosed rather than overclaimed or
fixed**: an earlier draft's own comments conflated two genuinely different real-world patterns under
one "commit" verb. The classic **best-of-N** pattern (Cursor's own `/best-of-n`: spawn N children
from the SAME still-unmoved base, evaluate, commit exactly ONE, discard the rest) commits with ZERO
conflict risk — `merge_trees(base, ours=base, theirs=child)` is a pure fast-forward when main has
not moved since the children were spawned, proven by a dedicated three-attempt check regardless of
which of the three is chosen. A DIFFERENT, also-real pattern — **sequential/interleaved commits**
of two branches that both modify the same path, where the second's `base` goes stale the moment the
first commits — correctly REJECTS rather than corrupts anything (§39.2's conflict check), but the
rejected branch's real work becomes addressable only through the lower-level A7 orphan-reclaim API,
never through this tool surface's own handle again. These are now named separately and precisely,
each with its own proof, rather than one conflated, overclaimed "closes it" comment.

**Two further items, disclosed rather than fixed, matching this document's own repeated "close what's
closable now, disclose the rest honestly" practice**: no capability-declaration design exists yet
for who may call these four verbs at all (unlike `RunShellTool`'s real `Capabilities<...>`
precedent — the constructor-injected `AsyncQuota` references likely ARE a sufficient runtime gate
under this project's "possession is authorization" idiom, but a declarative, host-auditable tag is
real, unbuilt work); and whether `commit_task_branch`/`discard_task_branch` should touch an
`AgentSession`'s own conversation/turn history is unaddressed — Claude Code's own `/rewind` (per
the 2026-08-27 research) treats file-restore and conversation-restore as independently selectable
by default, the strongest real precedent found, suggesting leaving these decoupled here too is the
industry-accepted choice, stated explicitly now rather than left to silence.

### 39.4 Full re-verification — 13/13 checks (3 new, proving the round-2 fixes), zero regressions across every other consumer

The fixed version was recompiled clean and rerun against a live Docker daemon: all 10 original
checks still pass, plus three new ones proving the fixes specifically — real `BranchCost` refund on
discard (check 11), a genuine three-attempt best-of-N committing with zero conflict risk regardless
of which attempt is chosen (check 12, distinct from and unaffected by the sequential-conflict check),
and true interleaved multi-handle usage (two task branches held open simultaneously in one session,
real work alternating between them, each branch's own state undisturbed by the other's — check 13).
Every other real consumer of `sandbox_runtime.hpp` (`probe_execution_surface.cpp`'s A3 suite,
`probe_mandatory_sandbox.cpp`'s A9 suite) was recompiled and rerun to completion with zero
regressions from the `merge_into`/`discard` additions or the `const`-reference retyping.

### 39.5 What this pass does and does not establish

**Established**: a real, adversarially-proven, tool-shaped surface for the git-worktree-per-task
pattern the 2026-08-27 use-case research found every actively-developed coding agent ships as its
primary isolation mechanism — not merely disconnected lower-level primitives, and not merely
designed on paper: 13 real checks against a live Docker daemon, three independent red-team rounds
converging on the same fatal finding from three different angles (a strong signal, not a lucky
guess), every real finding either fixed with a new proof or disclosed with the same precision this
whole document applies everywhere else. The stated composition with A9's `MandatorySandboxProvider`
is now real (a signature choice, not an assertion), closing a gap an independent pass found in this
same section's own first draft.

**NOT established**: wiring into the real, production `include/agentengine/` `Tool<>`/`AgentSession`
build — deliberately, matching A3/A9/every other A-numbered item's own scope boundary; a real
`cap::decl::TaskBranch<...>`-shaped capability declaration; conversation-history coupling (or its
deliberate absence) as anything more than a disclosed, reasoned choice; and — the standing caveat
every review round in this document carries forward — convergence for THIS pass, not exemption from
whatever the next one finds. **Update, §40**: "automatic reattachment of a conflict-rejected branch
to a fresh handle through this tool surface" — named above as NOT established — is now real; §40.2
closes it.

## 40. A8 and A10 fixes — closing two real gaps a fresh reconciliation pass surfaced, both adversarially re-verified

Prompted by explicit project-owner direction to continue prove-phase work after `ADR-100` (the
ADR-096/ADR-098 reconciliation, a separate, already-shipped-code line of work — see
`decisions/ADR-100-adr-096-098-sandbox-layering-reconciliation.md`) closed with a candid account of
its own first draft getting a causal claim backwards. That same session, re-reading this design's own
residuals with fresh eyes, found two real gaps neither A8 (§34.3) nor A10 (§39) had actually closed,
despite both looking finished on their own terms — the exact "two proven pieces, never checked
together" pattern this whole document keeps re-discovering (§26/§29, §32, §33), this time between an
already-shipped mechanism (A8's ACL cap) and a newly-built one (A10's task-branch tool) that had never
been traced against each other.

### 40.1 A8 — the ACL cap's real failure mode was worse than disclosed, and now has a real escape hatch

**The gap.** §34.3/§29.6's own text called `kMaxAclRootsPerDigest = 64` "a deliberately generous,
documented-not-tuned default... real usage data is needed before treating it as final" — true, but
incomplete. Tracing every real `insert_acl_root_bounded()` call site (`write()`, `commit()`,
`branch_from()` at §34.3's own line, `merge()`) against A10's own real calling pattern surfaced the
actual failure mode: this cap is not merely untuned, it is a PERMANENT, non-evictable ceiling (eviction
was already, deliberately, rejected — see §34.3's own text) with NO escape hatch at all. Once 64
distinct, non-descendant principals have ever touched one digest — the realistic driver being many
independent sessions forking from an identical, differently-owned SHARED base (a common onboarding
template, say) via `branch_from()`'s or `merge()`'s own ACL insertions — the 65th legitimate session is
denied forever, with no tuning knob available at runtime and no way for a content owner to say "this
is meant to be read by anyone." (A10's own dominant pattern — one session repeatedly forking from its
OWN already-owned main line — turns out NOT to trigger this: `insert_acl_root_bounded`'s own
already-existing "root id already present is a no-op" short-circuit means a session's own owner
principal, already a root on its own content, adds nothing new on repeat forks. The real driver is
cross-session sharing of a common, differently-owned base, a narrower but still real and plausible
production pattern.)

**The fix, two parts:**

1. **The cap is now a real, per-instance constructor parameter** (`Ledger(Store, durable_dir,
   max_acl_roots_per_digest = kMaxAclRootsPerDigest)`), not a compile-time-only constant — every
   existing call site (`Ledger<> ledger;`, `Ledger<>(store)`, `Ledger<>(store, durable_dir)`) is
   byte-for-byte unaffected via the new parameter's default.
2. **`mark_digest_shared(Digest, is_tree, requested_by)`**: an explicit, principal-gated escape hatch.
   `requested_by` must already pass the SAME `authorized_for()` check every read uses before they can
   mark a digest shared (I2: narrows/decides among authority `requested_by` already possesses, never
   mints new authority from nothing) — reuses the existing ACL `std::set<uint64_t>` itself as the
   storage, via a reserved sentinel id (`kPubliclySharedSentinelRootId = 0`, verified unreachable by
   any real principal: `IdentityAuthority` mints starting at 1 and `Principal` has no public
   constructor at all, `friend`-gated to `IdentityAuthority` alone) rather than a second, parallel data
   structure. `authorized_for()` grants any principal read access once the sentinel is present;
   `insert_acl_root_bounded()` becomes a genuine no-op for a publicly-shared digest — the real fix for
   the GROWTH vector, not merely a workaround for the denial: a publicly-shared digest's ACL set never
   grows again, fully exempt from the cap rather than merely allowed to exceed it once. PERMANENT,
   deliberately: no `unmark_digest_shared()` exists, matching this whole ACL mechanism's "no eviction,
   no silent revocation" posture with a one-way ratchet instead.

Real, adversarial proof, first run (`docs/planning/proofs/worktree_io/probe_acl_public_share.cpp`):

```
[1] a Ledger constructed with max_acl_roots_per_digest=2 admits exactly 2 distinct roots and rejects
the 3rd (ledger.acl_root_cap_exceeded) -- the cap is a REAL runtime constructor parameter -- PASS
[2] an UNRELATED principal ('stranger') cannot mark someone else's content shared
(ledger.mark_shared_unauthorized), and still cannot read it -- PASS
[3] the legitimate owner marked the digest publicly shared; a completely UNRELATED principal can now
read the REAL content back byte-for-byte -- PASS
[4] REAL EXEMPTION CONFIRMED: with max_acl_roots_per_digest=1 already exhausted, marking the digest
publicly shared lets 5 completely NEW, unrelated principals read it AND a 6th successfully
write-reference the same digest -- the cap is genuinely bypassed, not merely allowed to be exceeded
once -- PASS

ALL CHECKS PASSED
```

**Independent security/I2-I3/concurrency red-team round (2026-08-27, fresh reviewer)**: exhaustively
traced every `Principal`-minting path (not just `mint_root`) to confirm the sentinel is genuinely
unreachable, including the durable-restart path (`load_durable_state()` guards against a corrupted
high-water-mark reintroducing a low id); confirmed `authorized_for()`'s existing `it == acl.end() ->
false` early-return makes `mark_digest_shared()` structurally unable to create a fresh ACL entry for a
digest nobody legitimately wrote; confirmed the whole check-then-insert sequence runs under one
uninterrupted `mutex_` critical section (no TOCTOU); confirmed none of the five `insert_acl_root_bounded`
call sites depend on the skipped insert having grown the set. **One real, disclosed-not-fixed finding**:
unlike `HostSandboxSelection` (`sandbox_backend_registry.hpp`), `mark_digest_shared()`'s `Digest`
parameter has no structural, non-implicitly-constructible defense-in-depth against a future caller
passing a model-influenced value — dormant today (zero production callers; `requested_by` still needs
real, pre-existing authorization regardless of what `digest` names), named for whoever eventually wires
a real caller, not assumed safe by omission.

Full regression: every probe touching `worktree_ledger.hpp` (14 files: attack simulation, Docker
sandbox, case-collision, full-stack, crash-reclaim and durable-ledger read/write pairs — re-run in
correct process order after an earlier same-session mistake running a read before its write sibling was
caught and corrected — concurrent ACL, concurrent ledger, ledger-merge, worktree I/O) plus
`probe_execution_surface.cpp` and `probe_mandatory_sandbox.cpp` (both depend on `sandbox_runtime.hpp`,
touched in §40.2) — all green, zero regressions.

### 40.2 A10 — closing the "stranded loser" gap for real, using the already-proven A7 API

**The gap**, named honestly in §39's own header comment as "disclosed, not fixed": a rejected
`commit_task_branch()` (a real merge conflict, most commonly) erased the caller's handle before the
merge even ran and never re-surfaced anything on rejection — the branch's real work was NOT lost
(`Ledger::merge()` already registers every rejection into `orphaned_from_restart_`, §32.4's fix), but
was reachable only through the lower-level A7 `reclaim_orphaned_branch()` API, which this tool's own
caller has no path to.

**The fix**: a new `SandboxRuntime::reclaim_orphaned_child()` — a thin wrapper around the already-
proven, ACL-gated A7 API, returning a live, addressable `SandboxRuntime` again — and
`commit_task_branch()` now calls it on any merge rejection, re-inserting the reclaimed branch into
`active_` under the SAME `handle_id`. The caller still sees the original rejection error (e.g.
`ledger.merge_conflict`); what changes is that the handle keeps working afterward — retry, run more
work, or discard — through this tool's own surface, never forced to know about or reach the
lower-level Ledger API.

Real proof, following directly from §39's own conflict check (`probe_task_branch_tool.cpp`, check
`[9b]`, first run — after fixing one real ordering bug this pass introduced and caught by actually
running the probe, not by reasoning: the recovery check was originally placed at the end of the file,
where the reclaimed handle's continued liveness silently broke check `[13]`'s own hardcoded
`active_count() == 2` assertion by leaving an extra live handle in the table; moved immediately after
check `[9]` and cleaned up there instead):

```
[9] REAL MERGE CONFLICT: branch A commits cleanly (config.txt="FROM-A"); branch B's later commit of a
conflicting rewrite of the SAME file is REJECTED (ledger.merge_conflict) through this tool surface --
main's head still reads exactly "FROM-A" -- PASS
[9b] A10 FIX CONFIRMED: check [9]'s rejected commit (branch B) is NOT stranded -- its handle_id is
still live in THIS session's own table, its real work ('FROM-B') is still readable through a fresh
run_in_task_branch call on the SAME handle, and it discards cleanly through this tool's own surface --
the lower-level A7 orphan-reclaim API is never needed by a caller of this tool -- PASS
```

**Independent correctness/concurrency red-team round (2026-08-27, fresh reviewer)**: confirmed the
existing `exclusivity_` lock (held for `commit_task_branch()`'s full body since an earlier round's own
fatal finding) covers the whole reclaim-and-reinsert sequence with no new race; confirmed capturing
`branch_name` as a real `std::string` copy before `merge_into()`'s move is safe (a fully sequenced
statement, no dangling-reference hazard); confirmed a SECOND rejected commit on an already-reclaimed
handle correctly re-orphans and re-reclaims with no state corruption (`Ledger::merge()` unconditionally
re-registers into `orphaned_from_restart_` on every rejection, regardless of how many times the same
branch name has been through this cycle); confirmed the common path still returns the ORIGINAL
rejection error unchanged, no caller-visible ambiguity introduced. **One real, non-fatal observation**:
during the reclaim, the moved-from `child` and the freshly reclaimed `SandboxRuntime` briefly both
address the identical staging directory (same `compute_digest(branch_name)` path, since the name is
unchanged) — confirmed benign (`RealIoFileSystem` has no user-declared destructor and performs no
cleanup or cross-instance mutex aliasing on destruction) but non-obvious on a first read; noted in the
code, not fixed (nothing to fix — it isn't a bug).

Full regression: the full 13-check `probe_task_branch_tool.cpp` suite (now 14 checks with `[9b]`),
`probe_execution_surface.cpp`, and `probe_mandatory_sandbox.cpp` (all three real, live-Docker
consumers of `sandbox_runtime.hpp`) — all green.

### 40.3 What this pass does and does not establish

**Established**: A8's cap is now a real deployment knob with a real, adversarially-gated escape hatch
for legitimately shared content, closing a permanent-denial failure mode the original design
understated rather than merely re-disclosing it with a bigger number. A10's best-of-N-vs-sequential-
conflict distinction (§39's own finding 3) is now fully closed on the "stranded loser" side — a
rejected commit's real work stays reachable through this tool's own handle, not just through a lower
API this tool's caller cannot reach. Both fixes were built on a real, previously-uncovered interaction
this pass found by tracing call sites against each other, not by revisiting either A8 or A10 in
isolation — matching this document's own repeated lesson that convergence has to be checked between
pieces, not assumed from each piece's own standalone proof. Both got independent adversarial review
(not just self-verification) before being written up here, and both survived it with zero fatal
findings — a real, if modest, break from this document's own historical pattern where nearly every
independent round found something (§35.4's own honest expectation stands: don't read two clean rounds
as evidence the process has become more reliable, only as evidence these two specific fixes happened
to be right).

**NOT established**: whether 64 (now just the default, not a hard ceiling) is itself a reasonable
number for any real deployment — still real usage data this document does not have. Whether
`mark_digest_shared()` needs the same non-implicitly-constructible defense-in-depth
`HostSandboxSelection` has — named, not built, since there is no real caller yet to protect. Whether A8's
cross-session-sharing scenario (the actual driver, once isolated from A10's own dominant same-owner
pattern) is common enough in practice to matter — plausible, not measured. Production wiring for either
fix — unchanged from every other A-numbered item's own scope boundary, not attempted here.
