# ADR-156 — `agent.notes` write-back: a dedicated inbox ref, harvested into real `MemoryItem`s

**Status:** Proposed. Designed, red-teamed by an actual failing test (not merely reasoned about),
corrected, implemented, and proven — 18 checks (R1-R5) all pass against real in-memory object/ref
stores, and the full related memory-suite (8 targets) shows no regression. See §4/§5/§6. **Not yet
Judged.**

**Relates to:** `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.notes`'s spec: "ordinary writes into
`/memory`, landing as `AgentAuthored` `MemoryItem`s", `FsWrite<mount>`), GitHub issue #40 (`agent.notes`
has ZERO implementation — no mount, no conversion mechanism, nothing), `decisions/
ADR-153-agent-memory-codeact-bridging.md` §7 (named this exact write-back mechanism as its own
residual, reusing `harvest_mount()`), `029-Memory-System.md` §4 (the `AgentAuthored` `MemoryItem`
tagging this ADR produces), `include/agentengine/core/memory.hpp` (`write_memory_item`,
`ensure_memory_worktree` — the structural-write precedent this ADR's own `ensure_memory_notes_inbox_
worktree` mirrors), `src/backends/native_jail/agent_files_data_codegen.hpp` (the ALREADY-real generic
`open()`/`file_write` mechanism this ADR relies on rather than re-implements — see §2).

## 1. The question

Per issue #40: `agent.notes` has no implementation at all — not a codegen gap like `agent.progress`/
`agent.output` (026 §5's own table frames it as *ordinary file access*, not a Python module needing a
bootstrap), but a genuinely missing mechanism: nothing mounts `/memory` writable into a CodeAct
sandbox, and nothing converts a written file into a real `AgentAuthored` `MemoryItem` (029 §4) — not
even for the native, non-CodeAct surface.

**Stated so it has a wrong answer:** can a script's ordinary file write become a durable, discoverable
`MemoryItem` using only already-real primitives (`harvest_mount()`, `write_memory_item()`), reusing
`agent.files`' EXISTING generic file-write mechanism for the write itself (no new Python/worker code),
without the staging area ever colliding with — or being mistaken for — the structural storage
`MemoryProvider`'s own extraction path already writes to?

## 2. The design

**The "ordinary write" half needs NO new code.** `agent.files`' `open(path, mode)` (`agent_files_data_
codegen.hpp` → `_ae_internal.open`/`file_write`) already works against ANY `mount_id` present in a
session's `mount_roots` — `split_guest_path` parses `"/<mount_id>/<rest>"` generically, never a
hardcoded set of mount names. So once a host folds a staging directory into `mount_roots` under some
presentation name (e.g. `"notes"`), `open('/notes/todo.txt', 'w').write(...)` already works today,
unchanged — matching 026 §5's own "ordinary writes" framing literally, not merely in spirit.

**What's actually new:** making a written file DURABLE as a real `MemoryItem`. Two pieces:

1. **`prepare_memory_notes_mount()`** — creates a fresh, EMPTY real host directory per run (no read
   from the object store; this is a staging area, not a materialized snapshot).
2. **`harvest_memory_notes()`** — called by the host AFTER a run completes: `harvest_mount()` (already
   real, already tested — `worktree_mount_sync.hpp`) pulls every staged file back into a durable ref,
   then each one's REAL content (fetched via `object_store.get_blob()`) is wrapped into a `MemoryItem`
   (`source = agent_authored`) and committed via `write_memory_item()` against the STRUCTURAL memory
   mount — landing exactly where `MemoryProvider`'s own extraction path (029 §4) writes, so `recall`/
   `agent.memory` sees it like any other item.

## 3. Deliberately out of scope

- Reading prior notes back as plain files (`agent.memory`'s own `FsRead<mount>` half, ADR-153 —
  `agent.notes`' spec'd capability is `FsWrite<mount>` only; the read experience is `recall`).
- Live sync during a run (a write is durable only once `harvest_memory_notes()` runs, post-run,
  matching ADR-153's own one-shot precedent).
- Chunking/splitting a large note file into multiple `MemoryItem`s — one file, one item, matching
  026 §5's own "small and boring" bar; a script wanting several notes writes several files.

## 4. Red-team — a real design correction, caught by a failing test, not merely reasoned about

**First draft:** a `notes-inbox` SUBTREE of the SAME ref `memory_mount(principal)`/ADR-153's `recall`
already use (`Mount{mount_id, ref_name, "notes-inbox"}`), reasoning that `subtree_path` scoping alone
would keep it isolated from `write_memory_item()`'s own `<kind>/<id>` structural area.

**Wrong, caught by `test_memory_notes_write_back.cpp`'s own R2 check failing** with a real,
non-hypothetical error: `rank_memory_items()`/`list_memory_items()` (`memory.hpp`) walk the WHOLE mount
tree from `mount.subtree_path` — for the STRUCTURAL mount, `subtree_path == ""`, i.e. the ENTIRE ref —
and try to JSON-decode every leaf blob as a `MemoryItem` record. A subtree-only separation still leaves
raw note text (`"buy milk"`) sitting as a SIBLING of the structural JSON records under the same ref
root; `list_memory_items` choked on it: `json.malformed_number: expected a digit` — observed directly,
not predicted.

**Fix: a genuinely SEPARATE ref** (`memory_notes_inbox_ref_name(principal)`, its own `mount_id` too),
not merely a different subtree of the structural one. `list_memory_items`'s own tree walk starts from
`read_ref(ref_store, mount.ref_name)` — a different ref name never resolves into the inbox's tree at
all, regardless of any `subtree_path` value, closing the collision BY CONSTRUCTION rather than by a
scoping convention a future caller could get wrong. Needed its own bootstrap
(`ensure_memory_notes_inbox_worktree()`, mirroring `ensure_memory_worktree()`'s exact idempotent
shape), called internally by `harvest_memory_notes()` so callers never need a second manual bootstrap
step.

**A second, smaller finding, named rather than silently assumed away:** `cap::FsWrite`/`cap::FsRead`
match on bare `mount_id` only (`trust/capability.hpp`) — the SAME capability VALUE technically
authorizes either the inbox mount or the structural mount if a caller passed the wrong `Mount` object
to `mount_write`/`mount_read`. This is not a new hazard this ADR introduces (every subtree-scoped
`Mount` in this codebase has the identical property), and it is NOT what actually keeps a sandboxed
script out of the structural area — that guarantee is structural, not capability-level: a sandbox's own
`mount_roots` is only ever folded from `prepare_memory_notes_mount()`'s own inbox-ref-backed directory;
`memory_mount(principal)` (the structural mount) is only ever touched inside this file's own trusted,
host-side `harvest_memory_notes()`, never exposed to a sandboxed process at all. Documented in the
header, not structurally re-enforced — the SAME convention this codebase already applies to
`EffectContext::capabilities`/`bound_capabilities` ("borrowed; never owned here," enforced by
documentation).

## 5. Executed evidence

**Windows, MSVC (Visual Studio 18 Community), MSBuild, Debug, existing `build/` tree.** New target:

```
  ok: setup: the memory worktree bootstraps
  ok: R1: prepare_memory_notes_mount succeeds
  ok: R1: the returned mount name is the host-chosen presentation token
  ok: R1: a real, empty staging directory exists
  ok: R1: harvest_memory_notes succeeds
  ok: R1: two real files became two real MemoryItems
  ok: R1: each item's origin.source is agent_authored, not user_stated/model_inferred  (x2)
  ok: R1: each item carries the REAL run_id/turn_id the caller supplied, not a placeholder  (x2)
  ok: R1: both files' REAL content round-tripped, not stubs
  ok: R2: rank_memory_items succeeds against the structural mount
  ok: R2: the harvested note is discoverable through the SAME recall/rank_memory_items path every
        other memory item uses -- genuinely durable, not merely returned
  ok: R3 setup / R3: an empty staging directory harvests to zero MemoryItems, not a false positive
  ok: R4: a reserved mount name collision is refused / carries the specific error code / nothing written
  ok: R5 setup / R5: a later turn's harvest sees zero items -- genuine ref isolation
test_memory_notes_write_back: ALL PASS
```

**The must-fix finding verified non-vacuous**: R2 was OBSERVED TO FAIL (`json.malformed_number:
expected a digit`) against the shared-subtree first draft, then observed to pass once the dedicated-ref
fix was applied — a demonstrated closure, not merely an argued one.

**No regression** — the full memory test suite, rebuilt and rerun:

```
test_memory_worktree ..................... Passed
test_memory_cross_tenant_isolation ....... Passed
test_memory_provider ...................... Passed
test_memory_no_authority_laundering ....... Passed
test_memory_retrieval_determinism ......... Passed
test_memory_ranking_formula ............... Passed
test_memory_codeact_bridging .............. Passed  (ADR-153)
test_memory_notes_write_back .............. Passed  (this ADR)
100% tests passed, 0 tests failed out of 8
```

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| The "ordinary write" half needs no new code (reuses `agent.files`) | **CORRECT (by inspection of `split_guest_path`'s generic mount-id parsing)** | `agent_files_data_codegen.hpp`'s existing, already-tested `open()` |
| A staged file becomes a real, durable `AgentAuthored` `MemoryItem` | **CORRECT (after fix)** | R1/R2's checks |
| The harvested item is discoverable through the SAME read path as any other memory item | **CORRECT** | R2's `rank_memory_items` check |
| The inbox never collides with the structural `<kind>/<id>` area | **CORRECT (after fix; WRONG as first drafted, caught by a failing test)** | §4's own failure/fix narrative; R5 |
| No false positives on an empty staging directory | **CORRECT** | R3 |
| Reserved-name collision fails closed, nothing written | **CORRECT** | R4 |
| No regression to the rest of the memory subsystem | **CORRECT** | Full 8-target suite, 100% pass |

## 7. Residuals to name up front

- Ephemeral staging: a run that crashes before `harvest_memory_notes()` runs loses whatever was staged
  but never harvested.
- No end-to-end proof through a REAL running `NativeJailBackend` sandbox writing via `agent.files.open()`
  — this ADR proves `prepare_memory_notes_mount()`/`harvest_memory_notes()` directly (the same layer
  ADR-153's own tests operate at), not the full worker-process integration; wiring `mount_roots` +
  calling `harvest_memory_notes()` around a real `execute_code` call is a host-integration step, not
  attempted here.
- The capability-match residual named in §4 (bare `mount_id` matching, structural not capability-level
  isolation) — carried forward, not silently assumed stronger than it is.
- Reading prior notes back (`agent.memory`'s job, ADR-153) — untouched by this ADR.
