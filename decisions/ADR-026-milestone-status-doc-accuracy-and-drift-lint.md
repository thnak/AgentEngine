# ADR-026 — Milestone-status claims went stale in three independent places; add a lint

**Status:** Judged — 2026-08-10. Scope: corrects the flat "Milestone(s) N have not started" claim
in the three locations found to carry it, and ships a CI lint proven (positive control + tested
against its own corrected target text) to catch this exact class of drift going forward. Does not
attempt the deeper accuracy pass on `web/marketing/src/data/apiContent.ts`'s per-protocol notes and
RFC status list named in §5 — that is real, separately-scoped follow-up work, not silently folded
in here.
**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap 23 (the audit run that
surfaced this); `CLAUDE.md`'s own pre-existing anti-staleness rule ("Do not hardcode a
milestone/phase table here — it goes stale on the next commit"), which diagnosed this exact failure
mode two sentences before falling into it one paragraph later.

## 1. The question

`CLAUDE.md`, `README.md`, and the marketing site all state whether AgentEngine's milestones are
complete, in progress, or not started. **Stated so it has a wrong answer:** are all three of those
independently-maintained claims actually consistent with the real, landed code in
`docs/planning/milestone-N-*-breakdown.md`, or has the claim drifted in one or more of them?

## 2. Finding

All three had drifted, one worse than the other two:

- `CLAUDE.md:25` and `README.md:23-24` both stated "Milestones 7-9 have not started" — false:
  Milestone 7 (protocol conformance) had real, committed, tested code for MCP, A2A, AG-UI, and the
  declarative-format compilers (`docs/planning/milestone-7-protocol-conformance-breakdown.md`
  Phases A-F), plus a completed Phase G gate audit. Milestone 7 was in progress with its own exit
  criterion not yet met, not un-started.
- `web/marketing/src/components/ApiProtocolStatus.tsx:9-17` (rendered on the built marketing site,
  wired through `src/data/apiContent.ts`'s `protocolEntries`, last touched commit `7469883`,
  2026-08-07) was worse: it read "not a line of implementation or a test exists for any of them
  yet. Milestones 6–9, including Milestone 7 (protocol conformance), have not started" — wrongly
  folding in Milestone 6 (independently complete since before this claim was last edited) and
  flatly asserting zero implementation exists for surfaces that, by the time this claim was last
  touched, already had real headers (`json_rpc.hpp`, `server.hpp`, `client.hpp`, `agent_card.hpp`,
  `mapping.hpp`, `sse.hpp`, `projection.hpp`, …) under `include/agentengine/protocol/{mcp,a2a,agui}/`.
  This is public-facing marketing/API-docs content — arguably higher-stakes to get wrong than
  internal contributor guidance — and was untouched by the design's first-pass fix, which was
  scoped only to `CLAUDE.md`/`README.md`.

Verified directly against the current tree, not assumed from the audit that first surfaced it:
`grep -rn "have not started" CLAUDE.md README.md web/marketing/src/` returned exactly these three
hits, no more, no fewer.

## 3. Why this needed an ADR rather than a same-day silent patch

The text correction alone is a one-line judgment call. What made it non-trivial: a same-day silent
patch to just `CLAUDE.md`/`README.md` — which is what the first-pass proposal was — would have left
the worse-offending, more public copy of the identical bug untouched, and would have shipped a CI
lint whose own first specified regex (a paragraph-scoped `"Milestone(s)? N(-M)? ... have not
started"` match) self-collided with its own corrected target text: the corrected paragraph reads
"Milestone 7 ... is in progress: ... not yet met. Milestones 8-9 have not started." in one block,
and an un-scoped multi-line match anchored at "Milestone 7" could extend to the later, unrelated
"have not started" clause belonging to 8-9 — falsely re-flagging Milestone 7 and breaking CI on the
very commit that fixed the underlying problem. Both defects were found and named during design
review before either was shipped; fixing them together, with the fix's own positive/negative
control proven before merge, is what this ADR records.

## 4. The decision

**4a. Text corrections**, all three locations, replacing the flat done/not-started boolean with a
three-state description (matching `CLAUDE.md`'s own existing "milestones are complete / in progress
/ not started" framing, just made accurate):
- `CLAUDE.md`, `README.md`: Milestones 0-6 complete; Milestone 7 in progress, its own exit
  criterion (Phase G gate audit) not yet met; Milestones 8-9 not started.
- `web/marketing/src/components/ApiProtocolStatus.tsx`: same three-state correction, plus removing
  the "zero implemented"/"not a line of implementation" framing and the wrongful inclusion of
  Milestone 6.
- `README.md` also had a hardcoded `"15 ADRs"` in the same sentence (now 26) — corrected to link
  `decisions/README.md`'s own index rather than hardcode a second number that would go stale by the
  same mechanism this ADR exists to fix.

**4b. `tools/milestone_status_lint.py`** (new), modeled on `tools/naming_lint.py`'s existing,
already-CI-wired precedent: scans all three files above (a `SCANNED_FILES` list, meant to grow the
moment a fourth location is found) for a sentence containing "have not started", extracts every
milestone number named in *that same sentence* (sentence-scoped, not paragraph- or file-scoped —
see the script's own top comment for why that distinction is the actual fix, not a refinement), and
fails if any of those milestones has a real `docs/planning/milestone-N-*-breakdown.md` containing a
`**Outcome` marker (this project's own existing convention for a phase that actually shipped code).

**4c. Wired into `.github/workflows/ci.yml`** as its own job, `milestone-status-lint`, alongside the
existing `naming-lint` job, same `python tools/<script>.py` convention, no `continue-on-error`.

## 5. Falsifiable claims and verdicts

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| 1 | The lint, run against the ORIGINAL (uncorrected) text, actually catches all three stale claims — a real positive control, not a test that trivially passes | `git stash` the three doc corrections, ran the lint against the unmodified tree: exit 1, 5 findings — `CLAUDE.md` (Milestone 7), `README.md` (Milestones 6 and 7, both caught in one semicolon-joined sentence), `ApiProtocolStatus.tsx` (Milestones 6 and 7) — each citing the real breakdown file and its first `**Outcome` line as evidence | **CORRECT** |
| 2 | The lint, run against the CORRECTED text, does not self-collide (the exact bug found during design review) | Restored the corrections (`git stash pop`), ran the lint again against the same tree: exit 0, "no scanned file claims 'have not started' for a milestone that already has landed work" | **CORRECT** |
| 3 | The corrected `ApiProtocolStatus.tsx` no longer wrongly folds Milestone 6 into a not-started claim, and no longer asserts zero implementation exists | Read the corrected component directly: the paragraph now states Milestone 6 is complete, Milestone 7 is in progress with real code named, and only Milestones 8-9 are not started | **CORRECT** |
| 4 | The new lint is a real, required CI check, not an advisory one | `.github/workflows/ci.yml`'s new `milestone-status-lint` job runs `python tools/milestone_status_lint.py` with no `continue-on-error`, same convention as the pre-existing `naming-lint` job it sits beside | **CORRECT** |

## 6. Residuals — named, not silently deferred

- **`web/marketing/src/data/apiContent.ts`'s deeper content was found to be stale too, beyond what
  this ADR's cited red-team finding covered**, and is explicitly left unfixed here: `protocolEntries`
  (lines ~363-397) carries per-protocol notes like MCP's "No headers, no source, no tests" and A2A's
  "nothing implemented yet" — both false against the same real headers cited in §2 above — and the
  015 declarative-format entry claims "No parser exists," false against Milestone 7 Phase F's real,
  tested YAML-subset parser and Workflow/Agent document compilers. Separately, the RFC status list
  (lines ~413-419) marks RFC 014 (Workflow and Orchestration) as `status: "design"` despite Milestone
  6 — which is entirely about workflow orchestration — being complete. This is a real, comparable
  accuracy gap to the one this ADR fixes, but is a larger content-verification pass (five separate
  protocol notes plus a five-RFC status table, each needing its own current-truth check) than what
  was scoped, verified, and decided here — tracked as follow-up, not silently absorbed into this
  change's claimed scope.
- **The lint's sentence-scoping is regex/whitespace-based, not a real parser** (matching
  `naming_lint.py`'s own accepted trade-off, stated in this project's own review discipline: false
  negatives are the watch-for failure mode, not false positives). A rewording of "have not started"
  that doesn't match the literal phrase would silently stop being checked.
- **`SCANNED_FILES` is a hardcoded list of three paths.** A fourth location carrying the same class
  of claim (a new doc, a different marketing page) would not be caught until someone adds it to the
  list by hand — the same kind of staleness this ADR is fixing, one level up. No mechanism enforces
  keeping the list current; noted rather than solved.
