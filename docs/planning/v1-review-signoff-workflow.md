# The RFC review-to-implementation workflow

**Status:** Planning input, process doc — not a spec. It doesn't change any RFC's content or any
promotion gate; it defines the process an RFC (or a milestone's RFC cluster, per
[the roadmap](v1-implementation-roadmap.md)) goes through between "drafted" and "being implemented."
Written to work today, solo, and to keep working unchanged once the project takes outside
contributors — see §3 for where that distinction actually matters (it's narrower than it sounds).

Five stages: **Review & Feedback → Revision & Refinement → Sign-off / Approval → Work Breakdown &
Estimation → Handover & Kick-off.** Applies per RFC or per cluster; repeats for any post-v1 RFC
change too, not just the initial v1 pass.

```
 ┌────────────────┐   substantial change    ┌──────────────────────┐
 │ Review &        │◄────────────────────────│ Revision &            │
 │ Feedback        │                          │ Refinement            │
 └────────┬────────┘─────────────────────────►└──────────┬───────────┘
          │ no blocking findings                          │ findings resolved
          ▼                                                ▼
 ┌────────────────────┐   author/judge disagree   ┌──────────────────┐
 │ Sign-off /          │──────────────────────────►│ (back to          │
 │ Approval             │                          │ Revision)         │
 └────────┬────────────┘                          └───────────────────┘
          │ Reviewed (024 §4 ladder)
          ▼
 ┌──────────────────────┐        ▼ just-in-time, per milestone
 │ Work Breakdown &      │
 │ Estimation             │
 └────────┬────────────────┘
          │
          ▼
 ┌──────────────────────┐
 │ Handover & Kick-off    │
 └──────────────────────┘
```

## 1. Review & Feedback

**Trigger:** an RFC (or a milestone's cluster of RFCs, per the roadmap's groupings) is Draft and its
author believes it's complete.

**Actor — today (solo):** the project owner runs a structured adversarial pass against their own
draft. This isn't new — it's what the 2026-08-04 open-questions sweep already did (an Explore-agent
grep for unstruck `- **Q` bullets caught real gaps a first pass missed), and what the first live run
of this workflow does for the full RFC set (§5 below).

**Actor — once collaborators exist:** each cluster gets a **primary reviewer distinct from the RFC's
author**, assigned along the same domain boundaries the roadmap already uses (a security/capability
reviewer for 006–009, a protocol reviewer for 011–013/015, etc.) — assignment by domain fit, not
seniority. A solo author reviewing their own work is a strictly weaker check than an independent
reviewer; this is the one stage where headcount actually changes the mechanism, not just who signs.

**Artifact:** a findings list — severity (`blocking` / `major` / `minor`), the section it's about,
what's wrong, a suggested fix. `blocking` = contradicts I1–I8 or another RFC, or a promotion gate
that can't actually be falsified as written. `major` = underspecified enough that two reasonable
implementations would diverge in a way that matters. `minor` = everything else worth fixing.

**Exit:** every `blocking` finding is resolved or explicitly deferred with a reason (the same
strikethrough-and-resolve pattern `OpenQuestions.md` already uses) — not silently dropped.

## 2. Revision & Refinement

**Trigger:** a findings list from stage 1.

**Actor:** the RFC's author applies fixes. Disagreement between author and reviewer about whether a
finding is real doesn't loop indefinitely — it escalates to stage 3's judge, who breaks the tie as
part of sign-off rather than as a separate mechanism.

**Exit:** no unresolved `blocking` finding remains. A **substantial** revision (changes a promotion
gate, touches an invariant, changes a locked decision) goes back through a targeted stage-1 pass on
just the changed section — not a full re-review. A **narrow** revision (a fix that doesn't change
what the gate tests) proceeds straight to sign-off.

## 3. Sign-off / Approval

This is CLAUDE.md's existing Draft → Reviewed step (024 §4's status ladder), and where the
design→red-team→prove→judge track (CLAUDE.md, "contested, hot-path, or security-critical designs")
actually gets exercised for real, not just described.

**The rule, designed to work at any headcount — extending
[024 §7's deferred governance question](../../024-Versioning-Compatibility-and-Governance.md)
now that it's actually being asked, without inventing a quorum number 024 §7 correctly declined to
guess:**

- **Ordinary** change (doesn't touch I1–I8, doesn't add or remove a promotion-gate item, doesn't
  touch a CLAUDE.md-locked decision): author + one independent reviewer's approval is Reviewed.
  Solo today, this is the N=1 degenerate case — the owner holds both roles — which is exactly what
  024 §7 Q3 already resolved; nothing here contradicts it.
- **Contested / security-critical / invariant-touching** change: requires the full
  design→red-team→prove→judge cycle and lands as an ADR under `decisions/`, judged by a role
  distinct from the change's author *and* from whoever red-teamed it, to keep the adversarial
  separation real. Solo today, the owner holds author, red-team, and judge simultaneously — a real
  weakness of solo operation (self-red-teaming is strictly weaker than an independent adversary),
  named here rather than hidden, and not the permanent target.
- **The one rule that actually needs to exist for collaborators, and the only new thing this section
  adds:** once ≥ 2 maintainers exist, **the judge must not be the author of the change under
  judgment.** This is separation-of-duties, not a headcount quorum — it's meaningful at N=2 exactly
  as it is at N=20, so it doesn't need redefining as the project grows the way a fixed number would.
  A specific quorum size (two-maintainer approval, etc.) is still not invented here; that stays
  exactly as open as 024 §7 Q3 left it, deferred to whenever real contributor count makes a specific
  number meaningful rather than guessed.

**Exit:** RFC status moves Draft → Reviewed. `Proven` and `Accepted` still require the gate actually
running against real code (024 §4) — sign-off here means "ready to build against," not "done."

## 4. Work Breakdown & Estimation

**Trigger:** an RFC/cluster reaches Reviewed for the milestone about to start — not all nine
milestones at once. A breakdown written for Milestone 7 today would be stale by the time Milestones
1–6 actually finish and inform it, so this stage runs **just-in-time**, immediately before each
milestone begins.

**Granularity:** tasks come from the RFC's own `§Promotion gate` G-items plus the implementation
scaffolding each one needs — the gate is already a falsifiable unit of "done," so breakdown derives
tasks from it rather than inventing a separate task list that could drift from what the gate
actually checks.

**Estimation shape:** range/t-shirt (S / M / L / XL), never a point estimate. A numeric estimate for
genuinely novel work (WASM component sandboxing, exactly-once effect journaling under kill-injection)
would be exactly the false precision this project's own evidence-gated discipline already refuses
elsewhere (021 §7 Q2/Q4 stayed explicitly open rather than guessing a number without a backend to
measure against) — estimation shouldn't hold itself to a lower evidence bar than the spec does.

**Where it lives:** a per-milestone breakdown doc under `docs/planning/`, written when that milestone
starts. This project evaluated a live project-management tool for this and deliberately did not use
it — see §5's note. Migrates to issues/a tracker once a remote exists; not blocked on that existing.

## 5. Handover & Kick-off

**Trigger:** a breakdown exists for the milestone about to start.

**Solo today:** a short dated entry appended to that milestone's breakdown doc noting the start date
and any deviation from the roadmap's assumed order — the handover is from spec-work to
implementation-work, both done by the same person, so this stage is mostly a paper trail.

**Once implementer ≠ author:** the RFC + any ADRs it produced + the breakdown doc together *are* the
complete handover package — no separate briefing or tribal knowledge should be needed, which is the
whole premise of being spec-driven (CLAUDE.md's opening claim), actually exercised here rather than
just asserted. A short kickoff sync confirms the breakdown doc is sufficient on its own. If it isn't,
that's a signal the spec itself is underspecified for a reader who wasn't in the author's head — it
loops back to stage 1 for the specific gap, not a whole re-review.

**Exit:** the milestone is in progress; code is being written against a Reviewed spec.

## A note on tooling

This project has access to a live project-management system (`viot-tasks`) used for real client
delivery tracking at a Vietnamese dev shop — 14 active projects, real customers, real budgets. It was
checked for reuse here (2026-08-05) and deliberately **not** used: AgentEngine is unrelated internal
R&D with no client and no remote, and creating a project inside a live client-tracking system for it
would be the wrong home for it and visible to people it has nothing to do with. Stages 4–5 stay
repo-doc-based until this project has its own tracker, consistent with the earlier choice to draft
the roadmap itself as a repo doc rather than standing up a remote first.
