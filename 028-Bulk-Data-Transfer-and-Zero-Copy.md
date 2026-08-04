# 028 — Bulk Data Transfer and Zero-Copy

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 008, 010, 025, 026 · **Gate:** §8

## Goal

Move large data between the host, the sandbox, and tools **without serializing it to JSON, without
copying it three times, and without it ever transiting the context window** — while keeping the
agent-facing surface ordinary (026 §1) and the isolation contract intact (008 §2).

## 1. The problem, quantified

A tool returns 200 000 rows. Under a naive CodeAct implementation that value crosses the boundary as
a JSON array:

| Stage | Cost |
|---|---|
| Host serializes to JSON | 1 full pass + a large allocation; numbers become decimal text |
| Copy across the sandbox boundary | 1 more copy of the serialized bytes |
| Python `json.loads` | Parse pass + **allocation of one Python object per value** |
| Resident cost | A Python `int` is ~28 bytes and a `str` has ~49 bytes of overhead; a list of dicts multiplies that by keys-per-row. **An order of magnitude or more over the underlying data** |
| GC | Millions of small objects; collection pauses inside a wall-clocked sandbox |

The result is a sandbox that hits its memory limit on a dataset the host handled comfortably, and
spends most of its wall-clock budget parsing. Worse, if any of that data reaches the prompt, it also
costs tokens — the single most expensive resource in the system.

**Three rules follow, in priority order:**

1. **Bulk data never transits the prompt.** It moves by reference (003 §3, 025).
2. **Bulk data never transits as JSON** when a typed binary layout is available.
3. **Bulk data is not copied** when the profile can map it.

## 2. Mechanism 1 — handles, not materialization

The first and largest win costs nothing to implement: **a tool that produces bulk output returns a
reference, not the data.**

- A tool result above a configured threshold becomes a `BlobRef`/dataset handle (003 §3), written
  once into the worktree (025) and content-addressed. **§1's example is tabular, but the mechanism is
  general**: an oversized text read (a large log, a large source file) follows the identical path,
  per 006 §7's rule that the threshold is scaled to the run's token budget rather than a fixed byte
  constant — only the summary shown to the model differs (shape/columns here, a byte-count-plus-
  head/tail preview for arbitrary text).
- The model sees a small summary — shape, columns, a few sample rows — not the payload.
- The agent's code opens it when and if it needs it, and typically never materializes all of it:

```python
from agent import data

rows = data.open("search_results")          # lazy; nothing loaded yet
hits = rows.filter(rows.score > 0.8)        # pushed down, still lazy
print(hits.count(), hits.head(5))           # only this materializes
```

**This alone removes most of §1's cost**, because the common shape of agent work is *filter then
summarize*, and the filter can run where the data already is.

## 3. Mechanism 2 — a columnar binary interchange

For data that genuinely must be read, the canonical bulk format is **columnar and typed**, not JSON:

- **Apache Arrow IPC** as the interchange format of record. Chosen because its in-memory layout *is*
  its wire layout — a reader takes a view over the buffer rather than parsing it — it is
  language-agnostic (host C++, guest Python via `pyarrow`, plugins via the Arrow C data interface),
  and it is columnar, so a filter over one column touches one contiguous run.
- **Parquet** for at-rest storage in the worktree when compression matters more than mapping speed.
- **Newline-delimited JSON** as the interoperability fallback for tools that only speak JSON, with
  conversion at the boundary so the *sandbox* still receives Arrow.
- JSON remains correct for small results, and small is the common case. The threshold is
  configuration, and the conversion is invisible to the agent (§6).

**Arrow as a plugin, not a host dependency.** Per CONVENTIONS' dependency tiers and 009 §7, the
Arrow implementation is a candidate for the C/C++ plugin track rather than a linked host library —
which is also the answer for Parquet and compression codecs.

## 4. Mechanism 3 — zero-copy per profile

Whether the bytes are *copied* is a property of the profile, and — like everything else in 008 — the
**contract is identical while the mechanism differs**:

| Profile | Mechanism | Copies host→guest |
|---|---|---|
| `wasm` | Host writes directly into guest linear memory, or the guest reads through a host import returning a view. **memory64** (Wasm 3.0, 008 §1a) is what makes datasets above 4 GB possible at all | 1 (host → linear memory) |
| `native-jail` | **Read-only `mmap` of a worktree file** — POSIX `mmap` / Windows file mapping | 0 |
| `remote` | **Not possible.** Data streams over the transport | ≥ 1, plus network |

**The portable path is the memory-mapped worktree file**, and it is the default for a reason: 025
already stores bulk objects content-addressed and immutable, so mapping one read-only is safe by
construction — there is no writer to race with, which removes the entire TOCTOU class that shared
mutable memory would introduce.

**A `remote` sandbox cannot do zero-copy, and the spec says so** rather than pretending. Deployments
that move large data are deployments that should not choose `remote`, and the budget table (023)
makes the difference visible rather than mysterious.

## 5. Security constraints

Shared memory is a hole in an isolation boundary if it is done casually. The constraints are not
negotiable:

- **Host→guest mappings are read-only by default.** A writable shared region requires an explicit
  capability and a declared owner, and is never the default path.
- **No host pointers ever cross the boundary.** The guest receives offsets into its own address space
  or a handle, never an address meaningful to the host.
- **Regions are bounded and accounted** against the sandbox's memory limit (008 §2). A mapping that
  could exceed the limit is refused, not silently permitted — otherwise mapping becomes the way to
  evade the memory cap.
- **Immutable-by-digest for host→guest.** Because mapped objects are content-addressed (025), the
  host cannot mutate what the guest is reading. This is the mechanism, not a discipline.
- **Never shared across sessions.** A region belongs to one sandbox instance, one session, one
  principal. Reuse across sessions would be a covert channel and a data leak, and is prohibited in
  every profile (008 §6).
- **Zeroed or unmapped on teardown**, with no residue in a pooled instance (008 §9 G7).
- **Guest cannot resize, remap, or extend a region**, and cannot use a mapping to reach an object it
  does not hold a capability for.

## 6. What the agent sees

Nothing about any of this. Per 026 §1:

- `data.open(...)` returns something that behaves like an ordinary table — iterable, sliceable,
  `len()`-able, convertible with `.to_pandas()` when the profile has pandas.
- The agent does not choose a transfer mechanism, does not know whether the bytes were mapped or
  copied, and is never asked to reason about thresholds.
- Failures are ordinary: exceeding a limit is `MemoryError`, a missing dataset is
  `FileNotFoundError`.

**The one thing worth telling the model**, in one line of the tool's docstring, is that filtering
before materializing is cheaper — because that is *task* information, and it changes what code it
writes.

## 7. Interaction with the rest of the system

- **Replay (I5)**: content-addressed inputs mean a recorded run replays against the identical bytes,
  by digest, with no external fetch.
- **Audit (I4)**: a mapping is an effect — attributed, sized, and recorded like any other.
- **Checkpoints (019)**: a suspended run holds a *digest*, not a mapping; resumption re-maps. A
  suspended run must not pin memory (023 §4).
- **Cross-node (020)**: digests are location-independent, so a session migrating to another node
  re-maps from the store rather than shipping bytes with it.
- **Tools (006)**: a tool may declare that it produces bulk output, which is what lets the pipeline
  route the result to the worktree instead of through a message.

## 8. Promotion gate

- **G1 (the actual claim)** — a 10⁶-row, 10-column dataset is filtered and aggregated inside the
  sandbox on every local profile, within a declared memory ceiling and wall-clock budget. The naive
  JSON path is measured as the control and must be **materially worse** on both — if it is not, this
  RFC is unnecessary and should be deleted rather than defended.
- **G2 (zero-copy)** — under `native-jail`, the host-side copy count for a mapped dataset is **0**,
  measured, not asserted; under `wasm` it is exactly 1.
- **G3 (limits)** — a mapping that would exceed the sandbox memory limit is refused; a guest cannot
  use mapping to exceed its cap. Positive control: limits disabled, and the evasion succeeds.
- **G4 (isolation)** — a region from session A is unreachable from session B, including under
  pooling; canary bytes are never observed across the boundary.
- **G5 (immutability)** — a host attempt to mutate a mapped object while a guest reads it fails by
  construction; a deliberately mutable control demonstrates the race the design prevents.
- **G6 (fidelity)** — Arrow round-trip preserves types exactly across host, guest, and plugin,
  including nulls, timestamps with timezone, decimals, and nested lists — the four places where
  format conversions silently lose data.
- **G7 (surface)** — nothing in §4 or §5 appears in any agent-visible string or docstring (026 §8 G3).

## 9. Open questions

- ~~**Q1** — Whether Arrow is a plugin or a host dependency. Plugin is the stated preference (§3),
  but the host must at minimum *produce* Arrow buffers, which may pull a slice of it host-side
  anyway.~~ **Resolved, a minimal host-side IPC *writer* only, the full library stays a plugin
  (2026-08-04):** not a compromise of §3's position — writing a narrow, well-specified binary
  framing format (schema + record-batch messages) from already-trusted host data is a fundamentally
  smaller, safer surface than *reading* arbitrary/possibly-hostile Arrow bytes or running compute
  kernels over them, exactly the "hostile-input decode belongs in a plugin, producing ordinary
  structured output doesn't" line 009 §7 already draws. Reading, compute (filter/aggregate/convert)
  stay in the plugin, matching §2's `rows.filter(...)`/`hits.count()` examples, which already run
  inside the sandbox against the mapped bytes, not host-side.
- ~~**Q2** — Query pushdown depth: §2's example implies filter/projection pushdown into the host.
  How far that goes before it becomes a query engine we did not intend to write needs a boundary.~~
  **Resolved, stops at a single Arrow compute-kernel operation (2026-08-04):** the test — an
  operation belongs here if it's *one* compute-kernel invocation (filter, projection, simple
  aggregation) over an already-mapped buffer, with no need to materialize an intermediate result or
  coordinate across multiple datasets. Anything needing a query *planner* (join ordering, cost-based
  optimization) is explicitly out. An agent with a genuinely query-engine-shaped task uses DuckDB —
  already on 009 §7's candidate plugin list specifically for "a database dependency in the host
  process" — rather than this RFC growing one feature-by-feature, matching 026 §5's "small and
  boring" discipline applied here too.
- ~~**Q3** — Writable shared regions (guest→host bulk output) are stated as capability-gated and
  non-default; the actual protocol for them is unspecified, and it is the riskiest part of this
  RFC.~~ **Resolved by not building it — guest→host bulk output reuses the ordinary worktree write
  path (2026-08-04):** a live, concurrently-writable shared region needs real synchronization (locks,
  memory barriers, a handoff protocol) — exactly the kind of contested, security-critical design
  CLAUDE.md routes through design→red-team→prove→judge, not something to invent in a spec-editing
  pass. It isn't needed: the guest writes its output, incrementally or all at once, through an
  ordinary `FsWrite`-capable worktree mount (025), the same well-specified, already capability-gated
  mechanism every other guest write already uses; the host reads the resulting content-addressed
  blob when the guest is done. The accepted cost is asymmetry — host→guest gets true zero-copy via
  read-only `mmap` (§4), guest→host is an ordinary copy through the worktree — consistent with §4's
  own acceptance of real asymmetries (`remote`'s "not possible... ≥1, plus network") rather than
  forcing symmetry the mechanism doesn't need. §5's constraints already describe this path correctly,
  since a worktree write already has all of them.
- ~~**Q4** — Whether `remote` should transparently degrade (stream) or refuse bulk operations
  loudly. Transparent degradation is friendlier and hides a 100× performance cliff.~~ **Resolved,
  transparently degrade — confirming what §4 already states (2026-08-04):** §4's own table already
  answers the mechanism ("remote... Data streams over the transport") and already states the
  visibility answer too ("the budget table [023] makes the difference visible rather than
  mysterious"). Refusing loudly would make `remote` unable to do bulk work at all, worse than a
  slower-but-working path; the actual risk this question named (hiding a real cliff) is closed by
  023's operator-facing budget visibility, not by refusing the operation. The agent itself is never
  told (§8 G7, 026 §1) — this is architecture, invisible in-prompt like every other profile
  difference.
- ~~**Q5** — Whether tool *inputs* need the same treatment as tool outputs; the asymmetry is
  currently unjustified beyond "outputs are usually bigger".~~ **Resolved, Yes, symmetric — and
  already covered, not a gap (2026-08-04):** the apparent asymmetry was about which mechanism was
  described where, not an actual gap. §2's handle-passing mechanism is already symmetric by
  construction — 003 §3's `BlobRef` is usable in any content position, a tool *call*'s arguments
  exactly as much as a tool *result*'s, and §2's own example already shows a handle being *consumed*
  (`.filter()`, `.count()`), which is what an input is. §3/§4's Arrow-columnar/zero-copy machinery
  isn't input- or output-specific — it describes how any consumer of an already-content-addressed
  worktree object reads it, whether that consumer is agent code opening a handle or a tool invoked
  with a `BlobRef` argument that opens the same reference through the same path.
