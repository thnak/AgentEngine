# ADR-050 — `Usage`, not `UsageDetails`: reconciling 027 against 003/004 and shipped code

**Status:** Proposed (2026-08-14). Designed, verified via a real before/after lint run; awaiting the
project owner's explicit "Judged" sign-off per this project's governance (`decisions/README.md`;
`OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #9 (the finding this
ADR closes). `027-Vocabulary-and-Naming.md` §1-2 ("Normative for: every public identifier" — the
table this ADR corrects). `003-Message-and-Content-Model.md` §6, `004-Model-Provider-Plane.md`
(both already normatively spell it `Usage`). `decisions/ADR-025-naming-lint-namespace-scope-and-027-
vocabulary-diagram.md` §7 (explicitly deferred gap 9 to its own ADR rather than addressing it inline).

## 1. The question

**Stated so it has a wrong answer:** which of 027's table and the shipped `Usage` struct
(`core/content.hpp`) is the one that drifted?

**Before this fix: ambiguous by the audit's own framing** ("027's canonical name `UsageDetails` has
drifted to `Usage` in code" implies the CODE is what moved). Re-grounding against the actual
normative text shows the opposite: **003 §6 and 004 — both independently, both consistently — have
called this type `Usage` since those RFCs were first written**, not `UsageDetails`. 027's own table
row (§2, "Token and cost accounting (003 §6)") cites 003 §6 as its source while naming something 003
§6 itself never calls that. The naming-lint script (`tools/naming_lint.py`) confirmed this
structurally, not just by reading: `Usage` needed an `ae-naming-lint: allow` suppression comment to
avoid being flagged as unlisted, which is exactly the state a genuinely-drifted-from-its-own-spec
name would be in — except here the "spec" it drifted from (027's table) is itself the one row that
disagrees with the OTHER two normative documents governing the same field.

## 2. What re-grounding against current code found

- **The audit's own precondition for a mechanical fix is already satisfied, just in the opposite
  direction than its phrasing suggested.** Its own recommendation: "a mechanical rename only works
  once 003 §6 and 004 (which normatively say `Usage`, not `UsageDetails`) are reconciled too —
  otherwise it trades one spec/code drift for another." Re-reading 003 §6 and 004 directly (not
  trusting the audit's own paraphrase) confirms exactly what that recommendation already named: both
  RFCs say `Usage`, throughout, with zero occurrences of `UsageDetails` in either. Renaming the CODE
  to `UsageDetails` would create the exact three-way drift the audit's own recommendation warned
  against; correcting 027's ONE table row is the only direction that reconciles all three documents.
- **`Usage` is used extremely widely** (every `ChatResponse`, every backend's response parser, every
  test fixture that constructs a response) — a code-side rename would be large, mechanical, high-risk
  churn for zero semantic gain, precisely the kind of cost 027 §1's own naming rule doesn't ask for
  ("keep a different name only where the concept genuinely differs" — nothing here differs; MAF's
  `UsageDetails` and this project's `Usage` name the identical concept).
- **ADR-025 already named this exact question and deliberately deferred it** (§7: "Gap 9... a
  separate three-way RFC disagreement across 027/003/004... needing its own ADR — not addressed
  here"), confirming this is the intended, scoped follow-up, not a scope creep beyond what that ADR
  already anticipated.

## 3. The fix

`027-Vocabulary-and-Naming.md` §2's table row corrected from `UsageDetails` to `Usage`, marked
**ours** with an explicit "honest divergence" rationale per 027 §1's own required discipline (why
this project's name differs from MAF's, when the rule says adopt MAF's name by default): the shorter
name was already normatively established by 003/004 and already shipped as real, tested code — 027's
own table was the row that needed correcting, not the concept renamed away from MAF for a genuine
semantic reason. The now-unnecessary `ae-naming-lint: allow Usage` suppression comment on `core/
content.hpp`'s `struct Usage` is removed — it no longer needs one, since it now legitimately matches
027's own corrected table.

## 4. Self-red-team findings

**Checked, not assumed: no other file needed the same suppression comment removed.** Grepped for
every `ae-naming-lint: allow Usage` occurrence in `include/` — exactly one, the struct definition
itself. No risk of a second, now-stale suppression left behind elsewhere.

**Checked, not assumed: no CI gate hardcodes the specific suppressed/unlisted counts this change
shifts.** `tools/naming_lint.py` itself has no count-specific assertion (`.github/workflows/ci.yml`
just runs it and lets its own exit code decide); the 202-unlisted-name state (ADR-025's own
deliberately-deferred bulk reconciliation) is completely unaffected by this one-row correction —
verified by a real before/after run (§6), not merely inferred from reading the script.

## 5. What this ADR does not claim

- **Does not touch the 202 still-unlisted names** ADR-025 deliberately deferred — this ADR closes
  exactly gap 9's own scope, one table row, nothing broader.
- **Does not rename any code** — the entire point is that the code was already correctly named; only
  the vocabulary table's own row was wrong.
- **Does not audit 027 for other rows citing a normative RFC section that may have since drifted
  the OTHER direction** (a table row correctly matching code today but citing stale RFC section
  numbering, etc.) — that would be a separate, broader sweep, not this narrow, single-row fix.

## 6. Evidence

A real before/after run of `tools/naming_lint.py` (not merely reasoned about): before this change,
95 suppressed findings would have been reported plus `Usage` requiring its own suppression comment to
avoid appearing in the unlisted list (96 total suppressed, matching ADR-025's own last-measured
count); after removing the comment and correcting 027's table, the tool reports exactly 95 suppressed
findings (one fewer, `Usage` no longer needs suppression) and the SAME 202 unlisted names — proving
this change neither introduced a new violation nor silently absorbed part of the deferred bulk-
reconciliation backlog.
