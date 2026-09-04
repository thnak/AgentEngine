# ADR-169 — `rt::WorkflowSupervisor` admitted any caller that knew an `interaction_id`. Can that be closed without breaking ~200 existing call sites, and what is the right shape for a workflow-level admission gate?

- **Status:** Proposed — implemented, proven by real test execution (12 scenario groups, 60+ checks,
  every denial paired with an admission that must succeed), full 276-test suite re-run, pending
  project-owner sign-off.
- **Date:** 2026-09-04.
- **Scope:** `include/agentengine/rt/workflow_supervisor.hpp` (modified — new
  `workflow_status::admission_denied`; `caller` field on `RunWorkflow`/`ResumeWorkflow`/
  `ContinueWorkflow`; `set_principal()`/`principal()`/`set_require_caller()`/`require_caller()`/
  `admission_denied_count()`; private `admit_caller()`/`deny_admission()`/
  `propagate_admission_to_children()`; gates at all three entry points; caller forwarding and a new
  refusal branch in the ADR-157 nested-resume path; propagation hook in `bind_sub_workflow()`),
  `include/agentengine/rt/workflow_as_executor.hpp` (modified — one new `case` in the second
  exhaustive `switch(workflow_status)`),
  `tests/test_rt_workflow_supervisor_admission.cpp` (new),
  `tests/CMakeLists.txt` (additive wiring). **No other production file changed**, and no existing test
  file needed a single edit.
- **Related specs:** GitHub issue #65 (the defect this closes) ·
  `007-Capability-and-Trust-Model.md` §2 · `018-Identity-Authorization-and-Secrets.md` §1/§2/§6 ·
  `014-Workflow-and-Orchestration.md` §4 (the request port whose resume path this gates) ·
  `decisions/ADR-029-suspend-for-human-approval.md` (the check `AgentSession::resolve_interaction()`
  has had since Milestone 5 Phase H2 and this class did not) ·
  `decisions/ADR-061-host-provided-inbound-transport.md` §20.1/§20.4 (the two-mode admission shape,
  and why the host relaying an untrusted caller is the realistic threat model) ·
  `decisions/ADR-157-sub-workflow-nested-request-port.md` (the nested-resume branch this had to cover)
  · `decisions/ADR-070-host-configurable-responsibility-boundary.md` (the Delegated Decision Seam
  `require_caller()` sits in) · `docs/planning/batch-inference-coalescing-design-draft.md` (where the
  finding was first surfaced, as a side-finding of OQ-20's parked work).

## 1. The question

`WorkflowSupervisor::resume_workflow()` — the human-in-the-loop / `request_port` resume path — did no
caller or ownership check at all. `AgentSession`'s structurally identical entry points
(`start_run()`, `resolve_interaction()`) had gated exactly this since Milestone 5 Phase H2. Can that
asymmetry be closed without breaking the ~200 call sites across `tests/`, `examples/`, and
`include/` that construct these requests positionally today, and what is the correct shape for the
gate — identity-only, or ADR-061's full `RequestAuthority` bundle?

## 2. What was actually wrong

Three facts, re-confirmed against `main` on 2026-09-04 before any code was written:

1. **The request type carried no identity.** `ResumeWorkflow` was `{interaction_id, response,
   routes}`. There was nothing for an admission check to read even if one had been written.
2. **The entry point did no check.** `resume_workflow()` took the `AsyncMutex` (I1), checked
   `valid_`, then went straight to the `pending_sub_workflows_` / `ports_` lookup.
   `principal_admitted_for` had **zero** call sites anywhere in the file.
3. **`WorkflowSupervisor` had no owner `Principal` at all** — only the per-executor `contexts_`. There
   was nothing to admit *against*, which is why this was a small design and not a one-line patch.

What *was* guarded, stated precisely so the gap is not overstated: `resume_workflow()` already failed
closed on an unknown or already-resolved `interaction_id` (proven by E2 in
`tests/test_rt_workflow_supervisor_request_port.cpp`), and ADR-157's `pending_sub_workflows_` lookup
failed closed on a stale id rather than misrouting. The existing guard was **id validity, not
ownership**: knowing a live id was sufficient authority.

**Why that is authority, not a nuisance.** Resolving an interaction injects a `Message` into a
suspended run *and* names `routes` — on a `switch_case`/`multi_selection` edge that decides where the
run goes next. So the caller controls both what the workflow sees and where it goes, using effects
and budget belonging to the run's real owner (I2), attributed to nobody (I4). And interaction ids are
not secrets: they cross `WorkflowResult::open_interactions` to whatever host drives the run, and per
ADR-061 the host owning the inbound transport is precisely the layer that would otherwise be relaying
an untrusted caller's request.

**Honest severity.** This was a *latent* gap, not a live cross-tenant breach, for one reason only:
`contexts_` is not populated with genuinely distinct principals in any shipped code today. Anything
that changes that — OQ-20's batch coalescing, or any other multi-principal `request_port` use —
inherits an unchecked resume path. That is why this was filed and fixed separately from OQ-20, which
is parked under explicit project-owner direction.

## 3. Design

**One predicate, called first at every entry point.** `admit_caller()` is consulted immediately after
the I1 lock and *before* the `valid_` check, before any `ports_`/`pending_sub_workflows_` lookup, and
before any structural event. Dominating the `valid_` check is deliberate: an unadmitted caller must
not get a free, unattributed write into the host's observability stream, and must not learn whether
this supervisor holds a runnable graph.

**All three entry points, not just the one in the issue title.** `resume_workflow()` is the headline,
`continue_workflow()` was issue #65's own item 4, and `run_workflow()` was added here because gating
two of three would have left the same authority reachable through a different verb:
`run_workflow()` unconditionally clears `state_`/`ports_`/`pending_sub_workflows_`, so an unadmitted
fresh run against a *suspended* supervisor destroys exactly the interactions an unadmitted resume
would have hijacked (A8).

**A distinct status, not a reused one.** `workflow_status::admission_denied` is new.
`workflow_status::invalid` already means three unrelated things (graph never validated, unknown or
replayed `interaction_id`, a bound sub-workflow gone); folding a security decision into that bucket
would make a denial indistinguishable from a typo in an id — unusable for the audit surface I4
requires, and unusable for a host that wants to alert on one and not the other. It is handled in both
exhaustive `switch(workflow_status)` sites (`workflow_status_tag()` here, `status_tag()` in
`workflow_as_executor.hpp` — the same second switch ADR-159's own red-team pass caught).

**Asymmetric disclosure.** A denial returns a *bare* `WorkflowResult{admission_denied}` — no
`open_interactions`, no `partial`, no `output`. Populating `open_interactions` (which every other
early return in `resume_workflow()` does) would have turned the denial into an id-enumeration oracle
for the very authority the gate withholds. Meanwhile the *host* learns everything:
`admission_denied_count()` is incremented and a real `workflow_run_failed` event tagged
`"admission_denied"` is pushed to the event stream (A12).

**Source compatibility.** Each `caller` field is `std::optional<Principal>`, defaulted, appended last
— this project's established field-ordering convention. Every existing positional aggregate-init
(`RunWorkflow{input}`, `ResumeWorkflow{id, msg, routes}`, `ContinueWorkflow{}`) compiles and behaves
byte-for-byte unchanged, which is why not one existing test file needed editing.

**`Principal`, not `SessionCaller`.** `StartRun`/`ResolveInteraction` carry the narrower
`{id, tenant_id}` shape for a wire-shape reason `agent_session.hpp` documents, and it has a real cost
there: reconstructing `Principal{caller->id, caller->tenant_id}` **drops `on_behalf_of`**, so a
legitimately delegated principal (007 §2) is *not* admitted by that path even though
`principal_admitted_for()` would admit it. There is no wire shape here to be constrained by, so this
surface carries the identity the predicate actually reads, and single-hop delegation works (A5) —
matching `rt/multi_agent.hpp:191`'s full-`Principal` use rather than `agent_session.hpp`'s narrowing.

**Host configuration, not run state.** `principal_`/`require_caller_` are set by dedicated setters,
never by `initialize()` (which is legitimately called twice in ADR-157's bind sequence — a setting
reset by the second call would be a fail-*open* footgun of exactly the shape this ADR closes) and
never carried in `RunStateRecord` (same convention as `bodies_`/`contexts_`/`sub_workflows_`/
`designated_stall_reporter_`). Because they are object-level members neither `initialize()` nor
`restore_from_record()` touches, a configured supervisor keeps its gate across both — asserted
directly by A10, which is the positive control for the "checkpoint round-trip silently disarms the
gate" regression this shape could otherwise have introduced.

**Unset owner denies.** A default-constructed `principal_` plus a caller-bearing request is denied,
reached through the predicate rather than a special case: `principal_admitted_for()` needs a real
owner to admit against, and "no owner configured" must never read as "everyone is the owner" (A7).
This is the same behavior `AgentSession` already has for an unconfigured session.

## 4. Why identity-only — no `RequestAuthority` half

ADR-061 §20.1's `RequestAuthority` bundles identity *with* a `CapabilitySet` and an expiry. That is
deliberately **not** built here. A workflow's capabilities are per-**executor** (`contexts_`, fixed at
`initialize()` and checked against each node's `capability_ceiling` by `check_workflow_executable()`),
not per-request. A per-request `CapabilitySet` would need a defined interaction with that per-executor
ceiling that nothing in 014 or 007 specifies today — and accepting the capabilities while ignoring
them would be worse than not having the field: it would read, at every call site, as a guarantee that
does not exist. Named as an open extension, not silently dropped.

The half of §20.4 that *does* transfer is the mode branch. §20.4's literal rule ("no code path reads
both `caller` and `authority`") is vacuous with one identity field; what survives is the part that
matters, and the part §20.4 records as having broken once already: a `require_caller_` supervisor
**never** admits an identity-less request by falling through to the permissive branch. Written as an
explicit early return, not a widened `||`.

## 5. Delegated decision: `require_caller()` defaults OFF

`set_require_caller(true)` makes an identity-less request a hard refusal. It defaults **off**, and
that is an ADR-070 Delegated Decision Seam, not an oversight:

- Flipping the default would break every existing call site while proving nothing about the
  mechanism — the engine cannot tell an in-process host driving its own trusted supervisor from one
  fronting it for untrusted callers, and guessing wrong in the strict direction breaks working code
  rather than protecting anyone.
- It satisfies the seam's conditions: **explicit host opt-in**; **fails closed once set** (A6 proves a
  `require_caller()` supervisor refuses both an identity-less run and an identity-less resume);
  **narrows, never widens** (it can only turn admissions into denials); **host code, never model
  output** (I3 — the setter is called by the host binary, and nothing derived from a model reaches
  it); **always audited** (`admission_denied_count()` plus the event).

The residual is stated plainly: a host that configures neither a principal nor `require_caller()` is
exactly as exposed as it was before this ADR. What changed is that it now *can* close the gap, that
doing so is a two-line call, and that the gap is documented rather than invisible.

## 6. Nested sub-workflows (ADR-157)

Issue #65's item 5 required that the inner supervisor not be treated as already-admitted by
construction. `resume_workflow()` therefore **forwards** `request.caller` into
`inner->resume_workflow()`, so the inner gate genuinely runs and the nested effect stays attributed to
the real caller (I4) rather than to "the outer supervisor".

That alone would have broken nested HITL for every host that reasonably configured only the root: an
un-owned child would deny every forwarded caller. `propagate_admission_to_children()` closes that —
a bound child with no owner of its own inherits the parent's, recursively, bounded by
`kMaxNestingDepth`. It runs from `bind_sub_workflow()` *and* from both setters, so
configure-then-bind and bind-then-configure converge on the same state and a host need not know which
ordering it used (A11a, A11b). A child the host *did* give a distinct owner keeps it and gates
independently (A11c) — a real, documented cost: such a host must make its callers admissible at both
levels.

This is not ambient authority. The child's gate still genuinely runs; inheritance only supplies the
owner it runs against, and it is the same "derived, never elevated" shape 007 §2 already uses.

## 7. Two real defects found by execution, not by review

Both were caught by tests failing on the first run, and both are recorded here because the design as
written on paper contained neither.

**(a) Propagation was not idempotent across two configuration calls (A11b).** The first implementation
decided "does this child already own its config?" by testing whether the child's principal was empty.
After `set_principal()` propagated, the child was no longer empty — so a subsequent
`set_require_caller()` skipped it, and the child silently kept `require_caller_ == false` while its
parent required a caller. A strictly fail-*open* divergence. Fixed with an explicit
`inherited_admission_` flag distinguishing "owner arrived by inheritance" (overwritable, keeps
tracking the parent) from "owner set explicitly on this object" (never overwritten); either setter
called directly on a child clears it, because an explicit host choice outranks inheritance from that
point on.

**(b) A nested denial was laundered into a completed run, and destroyed the interaction (A11c).** The
ADR-157 nested branch converts any non-`completed` inner outcome into a `failure_marker()` message and
routes it onward as an ordinary port resolution — correct for a real inner run *failure* (ADR-157's
own S4 shape), badly wrong for a *refusal*. The outer reported `workflow_status::completed` while
carrying a poisoned payload, and — worse — `pending_sub_workflows_` had already been erased before the
inner call, so the nested interaction was destroyed by an attempt that decided nothing: an
irrecoverable denial-of-service against a legitimate outer caller who simply was not admissible at the
inner. Fixed by restoring the pending entry verbatim and surfacing the refusal as itself. The outer
deliberately does **not** increment its own `admission_denied_count_` on this path — its gate admitted
this caller; the refusal happened at the inner's gate and is counted there, which is where a host
debugging it needs to look. The structural event is still pushed on the outer's stream.

## 8. Evidence

`tests/test_rt_workflow_supervisor_admission.cpp`, A1–A12. Every denial assertion is paired with an
admission against the *same* supervisor that must succeed, and every case asserts
`admission_denied_count()` — a gate that "denies" by doing nothing observable cannot pass this file.

| | claim |
|---|---|
| A1 | the legacy shape (no owner, no caller) runs, resumes and completes exactly as before; denial count 0 — the positive control for ~200 existing call sites |
| A2/A3 | the owner is admitted on the same interaction a stranger was just denied on; the denial leaks no `open_interactions`/`partial`/`output`, is distinguishable from `invalid`, is counted, and consumed nothing |
| A4 | same principal id in a different tenant is denied (018 §6) |
| A5 | a single-hop `on_behalf_of` delegate is admitted; a two-hop one is not |
| A6 | `require_caller()` refuses an identity-less run *and* an identity-less resume outright; the refused run executed 0 rounds |
| A7 | an unconfigured supervisor denies a named caller |
| A8 | an unadmitted `run_workflow()` against a suspended supervisor is refused and does not reset the live interaction, which is still resumable afterwards |
| A9 | `continue_workflow()` is gated identically and leaks no ids |
| A10 | the gate survives `initialize()` called twice *and* a `to_record()`/`restore_from_record()` round-trip |
| A11 | nested: forwarding, inheritance in both orderings, non-overwrite of an explicitly-owned child, and §7(b)'s refusal semantics |
| A12 | I4: the denial reaches the host's event stream as `workflow_run_failed` tagged `"admission_denied"` |

**Build and suite.** Clang 21 / Ninja / Debug, `agentengine_warnings` (this project's `-Werror`
configuration): zero warnings, zero errors on a full rebuild. Full `ctest -j8`: 275/276 passed. The one
failure, `test_rt_spawn_cost_budget`, is unrelated and pre-existing — it includes only
`rt/spawn_cost_budget.hpp` (untouched by this ADR), is a `std::thread`/`std::atomic` timing test, and
passes standalone; a load flake under `-j8`, not a regression. `tools/naming_lint.py` and
`tools/milestone_status_lint.py` both OK.

## 9. What this explicitly does NOT do

- **Does not make any host safe by default.** `require_caller()` is off unless a host turns it on
  (§5). A host that configures nothing is exactly as exposed as before; it can now close the gap, and
  the gap is documented.
- **Does not build the `RequestAuthority` capability half** (§4) — no per-request `CapabilitySet`, no
  expiry, no bearer-credential bridge. Named open extension.
- **Does not plumb identity through `WorkflowChatClient`** (`rt/workflow_as_chat_client.hpp:355`
  resumes with no caller). Harmless today (that adapter's inner supervisor is never given an owner),
  but it means a host cannot yet put a gated supervisor behind that adapter and have callers reach it.
  Named, not silently left to be discovered.
- **Does not persist the admission configuration in `RunStateRecord`** (§3). A restored supervisor's
  host re-supplies it, like every other piece of host configuration; A10 proves the ordering that
  makes this safe, and nothing makes it *automatic*.
- **Does not add per-interaction ownership.** The unit of admission is the supervisor, not the
  individual `OpenPort`. A workflow whose distinct interactions genuinely belong to distinct
  principals — the multi-principal shape §2 names as the direction of travel — needs a further design;
  this ADR makes such a workflow's resumes gated, not correctly *partitioned*.
- **Was not run through the full `design → red-team → prove → judge` process.** The mechanism reuses
  an already-shipped, already-red-teamed predicate (`principal_admitted_for()`) at a new call site
  rather than inventing an authority primitive. The adversarial work that did happen is §7's two
  execution-found defects, both fixed, both now regression-tested.

## 10. Promotion gate

**G1 (met).** A caller holding a valid `interaction_id` but not admitted for the supervisor's owning
principal cannot resolve, continue, or restart the run, cannot learn any other interaction's id from
the refusal, and cannot destroy the interaction it was refused — while the owning principal and a
single-hop delegate can still do all three. Falsifiable, and it does fail: reverting either gate makes
A3/A6/A8/A9 fail, and reverting §7(b)'s branch makes A11c fail.

**G2 (met).** No existing call site changed behavior: the full pre-existing suite passes with zero
edits to any existing test file.

**G3 (open, for the project owner).** Should `require_caller()` default ON in a future major version,
accepting the migration cost across every embedding host? Not decided here; §5 records why it defaults
off today.
