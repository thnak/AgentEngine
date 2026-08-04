M2 Phase F — cross-cutting ADR-track tasks (egress proxy, policy-reachability tool)

## Context

The roadmap explicitly flags two items for M2 that are ADR-track (design → red-team → prove →
judge per CLAUDE.md), not ordinary implementation tasks — the same discipline Phase A's
[ADR-009](../../decisions/ADR-009-capability-set-enforcement-mechanism.md) already went through for
capability enforcement. Both are security-critical: `decisions/README.md`'s own rule is that any
"security-critical choice: capability representation, isolation boundary, taint mechanism, approval
binding, secret handling" requires a full ADR, not an ad-hoc change.

These two tasks are independent of Phases C/D/E and can be scheduled in parallel with them or
after, at the project owner's discretion — they're grouped here because the roadmap groups them,
not because they block or are blocked by the other phases.

## Tasks

- **F1. First-party egress proxy design** (008 §10 Q3) — full design→red-team→prove→judge cycle,
  produces a new ADR. Security-critical: host-mediated network egress for every sandbox profile
  depends on getting this right — a bypassable egress boundary would undermine every capability
  check gating `NetOut` (007 §3) built on top of it. **Size: XL**
- **F2. Policy-reachability tool** (007 §9 G6) — new CI tooling enumerating `{capability kind,
  tool, taint level}` against whatever mechanical enforcement Phase A's ADR-009 actually
  implements (scoped to mechanical possession/attenuation, not the full 007 §5 declarative policy
  language, per Phase B's decision 4). **Size: L**

## Exit criteria

- **F1**: an ADR under `decisions/` recording the egress-proxy design, its red-team pass, executed
  evidence (positive controls, not just described-as-prevented), per-claim verdicts, and the
  accepted decision with residual risks — matching ADR-009's own structure.
- **F2**: a CI-runnable tool that enumerates the actual `{capability kind, tool, taint level}`
  reachability graph against the real mechanical enforcement in `trust/capability.hpp` /
  `core/tool_pipeline.hpp`, catching (with a demonstrated positive control, not just a described
  intent) at least one class of over-broad reachability a manual review would miss.
