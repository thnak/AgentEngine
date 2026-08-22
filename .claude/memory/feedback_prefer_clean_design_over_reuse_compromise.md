---
name: feedback-prefer-clean-design-over-reuse-compromise
description: "In AgentEngine design work, prioritize clean/coherent design over preserving existing types or minimizing diff — don't let backward-compat or reuse-for-its-own-sake drive the shape of a new design."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: ed020a12-6d67-4381-9925-ebcd98051588
  modified: 2026-08-22T03:31:14.773Z
---

When designing new AgentEngine subsystems (design drafts, ADR-track proposals), do not treat "reuse the
existing type/mechanism" or "stay additive so nothing existing breaks" as a load-bearing constraint by
default. Prioritize the cleaner, more coherent solution even when it means proposing to change or replace
an existing type/mechanism, not just add beside it.

**Why:** the user (project owner) stated this explicitly, unprompted, as standing guidance
(2026-08-22): future maintenance burden from compromise-driven designs, or from under-valuing a needed
refactor/redesign in order to preserve something existing, is worse than the cost of redesigning cleanly
now.

**How to apply:** when a design draft under `docs/planning/*-design-draft.md` finds that an existing
type doesn't cleanly fit new requirements (e.g. a struct with a fixed field shape, a flat capability-bool
bag, an enum that doesn't cover the new case), the default move is to propose changing/replacing it and
name the migration cost honestly — not to work around it with an additive-only field or a parallel
mechanism purely to avoid touching existing call sites. Still name the migration cost/blast radius
explicitly (existing project discipline — CLAUDE.md's own "every RFC has a promotion gate," red-team
passes still apply), but don't let "avoid breaking an existing call site" alone be the deciding factor
against an otherwise cleaner shape.

This does not override CLAUDE.md's actual locked decisions (I1-I8 invariants, ADR-recorded decisions) —
those still bind. It's specifically about design *taste* on open questions: reuse vs. redesign, additive
field vs. reshaped type. See also [[unified-streaming-design-draft-context]] if that memory exists — this
preference surfaced while reviewing that draft's own additive-only choices (e.g. keeping `ModelDelta.
text_delta` unchanged specifically to avoid breaking `cli_chat.cpp:928`).
