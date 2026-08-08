# Milestone 6 residuals carried forward to M7/M9 — dependency note

**Status:** Pre-milestone scoping note, not a stage-4 work breakdown — same rationale as
`backgroundable-standingeffect-gap.md`: M7 has no owning-RFC-cluster reached-Reviewed-for-the-milestone-
about-to-start trigger yet (`v1-review-signoff-workflow.md` §4), since M6 (which this note is
downstream of) is still in progress in a concurrent session as of 2026-08-08. This note exists so
M7/M9's kickoff sessions don't have to re-derive these dependencies by reading M6's breakdown doc
line by line — it consolidates what M6 itself already named as deferred, with citations, and closes
one open assignment question M6 explicitly left unresolved (below).

**Source:** `milestone-6-multi-agent-orchestration-breakdown.md`'s "Design decisions" (numbered 1-11)
and "What this milestone will not close, stated up front" sections, both read directly as of M6
Phase G (`5289c42`) — the latest commit on that doc as of this note. M6 Phases H-J (030 Project
layer) are in progress in another session concurrently; this note covers only what Phases A-G
already committed to in writing. **Re-check the breakdown doc's own two sections before treating
this note as current** if significant time has passed or Phases H-J have landed — they may add
their own deferred items this note doesn't yet know about.

## What M6 explicitly built as a shape, with the surface/loader left for later

### 1. 014 §4's request port: engine half built, four surfaces deferred to M7 (decision 1)

M6 Phase E built the *shape* — `InputRequired` emission, `Interaction` records, suspend holding no
resources, resume on response — proven against a real `Engine` (014 §8 G5, half-proven: activation
census passes, "resumes after process restart" needed Phase F's checkpointing, since proven in full
per Phase F's own outcome note). What's deferred is binding that shape to **013's four surfaces**
(AG-UI, A2A, MCP MRTR, OpenAI-compatible SSE) — 014 §4 itself frames this as "one shape, four
surfaces (013 §5)," and M6's decision 1 built the shape specifically so M7 only has to bind
surfaces, not invent the suspend/resume mechanism from scratch.

**For M7:** 013's own RFC work (streaming/UI surfaces) has a concrete, already-proven engine
primitive to project rather than a paper design — `RunWorkflow`/`ResumeWorkflow`'s suspended-reply
shape and the `Interaction` record are real code as of Phase E/F, not something 013's kickoff needs
to first confirm exists.

### 2. 014 §1's typed-edge validator: built shared, declarative loader deferred to M7 (decision 6)

014 §1 requires an incompatible edge to fail "at compile time for the C++ form, at load for the
declarative form (015), using the same validator (I6)." M6 built the validator as a shared,
non-template-only predicate over the graph description specifically so 015's loader can call the
same code rather than a parallel reimplementation. 014 §8 G4 ("in both the C++ and declarative
form") is **half-provable** as of M6 — the C++ half is proven, the declarative half needs 015.

**For M7:** the validator-sharing work is already done; 015's loader is the remaining piece, not a
new validator. Building a second, parallel validator here would silently break I6's equivalence
claim and contradict M6's own stated reason for building it as a shared predicate — the loader
needs to *call* M6's validator, not re-derive its rules.

## What M6 explicitly deferred to M9, with the load-bearing part already built (decision 2)

### 3. 030 §6's `EmbeddedHost` facade: deferred to M9, its four verbs built as engine operations now

`create_project`/`list_projects` (ordinary asks against the Project-registry actor) and
`pause_project`/`restore_project` (acting directly on the named Project's own supervising actor,
deliberately **not** through the registry) are built in M6 Phases H-I as engine-level operations.
No `EmbeddedHost` type exists yet — that belongs to 020 (M9) — but 030 §6 itself is explicit these
four verbs are not a new protocol surface, and the registry/direct-actor split is exactly what makes
030 §7 G1 ("pausing one Project has zero observable effect on the other N-1") hold. M9 adds a facade
over already-correct behavior, not a redesign.

**For M9:** `v1-implementation-roadmap.md`'s current M9 section does not name this dependency
explicitly (as of this note) — it should, the same way M7's section now names 006 §6b, so a future
M9 kickoff doesn't have to rediscover that the four verbs it's facading already exist and are
already proven (030 §7 G1, via M6 Phase J's exit-criterion proof) before M9 starts.

## An M6-named residual with no milestone assignment — resolved by this note

### 4. 014 §8 G6 (cross-node checkpoint consistency, ≥3 cluster nodes): assigned to M9

M6's decision 7 names this explicitly as deferred, not silently dropped — "Quark has real
`cluster.hpp`/`membership.hpp`/`placement.hpp`, so this is not impossible — but standing up a ≥3-node
cluster with an injected node failure between checkpoint-pending and checkpoint-committed is a
distinct piece of infrastructure this repo has never built, and the roadmap's own M6 exit criterion
does not include G6." The two-phase pending→committed discipline G6 exercises is built and proven
**single-node** in Phase F; what's missing is multi-node execution of the same discipline. Unlike
items 1-3 above, M6's breakdown doc does not say which later milestone picks this up — it only says
"deferred."

**Resolved here:** `020-Configuration-and-Hosting.md` §3a's hosting-shape table names a **Cluster**
hosting shape explicitly — "N nodes over Quark's cluster (010/021/026) with sessions placed by
HRW/VirtualBins" — as one of five shapes 020 covers, and 020 is an **M9** RFC. A ≥3-node cluster
with injected node failure is not buildable as a standalone test harness before the Cluster hosting
shape itself exists and is exercised; building throwaway cluster-test infra ahead of 020 would mean
maintaining two multi-node harnesses (a disposable one now, 020's real one later) rather than one.
**Assigning 014 §8 G6 to M9**, to be proven once 020's Cluster hosting shape is real, is therefore
the same reasoning already applied to 006 §6b's M7 assignment: build the gate against real
infrastructure, not a stub that has to be reconciled later.

`v1-implementation-roadmap.md`'s M9 section should name this the same way M7's section now names
006 §6b's rationale.

**Discrepancy, now resolved:** M6's own closing summary (Phase J, commit `9a66e4c`, "Milestone 6 is
complete") originally grouped this residual with M7's — "M7 (015's declarative loader, 013's four
HITL surfaces, the ≥3-node cluster claim)" — with no cited rationale; M6's own itemized bullet for
this residual (decision 7, quoted above) never named a milestone either way, only that trailing
summary sentence did. Corrected in that doc to read M9, with a one-line pointer back to this note's
reasoning: 020 is the only RFC in the roadmap that owns a cluster/multi-node hosting shape at all,
and none of M7's RFCs (011/012/013/015) are about deployment topology.

## One item M6 named but did not attempt to place — stays unassigned here too

### 5. Map-reduce's data-driven fan-out cardinality (§3 row 6) — no milestone claims it, deliberately left open by this note

M6 Phase C built Map-reduce over a *fixed* mapper set; what's not built is *K mappers for K items, K
known only at runtime*. 014 §9 Q3 already resolved this as "separable... per-node instance
multiplicity, not a graph feature" — a smaller, more self-contained gap than items 1-4 above (no
RFC beyond 014 itself is involved, no cross-cutting infrastructure like a cluster is needed). This
note flags it for visibility but does not propose an owning milestone — unlike G6, there's no
natural single home for it (it could land as a small addition whenever 014-adjacent work is next
touched, including possibly inside M7's own declarative-loader work if the loader needs to express
runtime-determined fan-out counts, or as a standalone follow-up). Left open intentionally rather
than force-assigned.

## Summary table

| # | Residual | Source (M6 decision/section) | Depends on | Assigned to |
|---|---|---|---|---|
| 1 | 014 §4's four HITL surfaces | decision 1 | 013 | M7 (013 is already an M7 RFC) |
| 2 | 015's declarative-edge loader | decision 6 | 015 | M7 (015 is already an M7 RFC) |
| 3 | 030 §6 `EmbeddedHost` facade | decision 2 | 020 | M9 (020 is already an M9 RFC) — roadmap text should name it explicitly |
| 4 | 014 §8 G6 (≥3-node cluster checkpoint consistency) | decision 7 | 020's Cluster hosting shape | **M9 — newly assigned by this note**, roadmap text should name it |
| 5 | Map-reduce runtime-determined fan-out cardinality | Phase C outcome note | none cross-cutting | **Unassigned, deliberately** |

Also cross-referencing the pre-existing, separately tracked residual this note does not duplicate:
`backgroundable-standingeffect-gap.md` (006 §6b / 019 §2, assigned to M7, 2026-08-08) — the largest
of M7's inherited dependencies, already fully documented there.
