---
name: project-adr003-status
description: ADR-003 caller-aware import gating (OQ-15) is judged and committed; open thread on vendoring CPython from source
metadata: 
  node_type: memory
  type: project
  originSessionId: 4ae972bf-7b88-4e4e-b13c-cc93510fe90f
  modified: 2026-08-01T11:42:51.987Z
---

ADR-003 (`decisions/ADR-003-caller-aware-import-gating.md`) completed the full
`design → red-team → prove → judge` cycle and is committed as `cf38d96`
("ADR-003: prove and judge caller-aware import gating (resolves OQ-15)"), 2026-08-01.

As of 2026-08-01 the repo has a remote: `origin` → `https://github.com/thnak/AgentEngine.git`,
`main` tracking `origin/main`. Before this it was local-only (no remote) — if you see stale
references elsewhere assuming "no remote," this is the correction.

**What's resolved:** OQ-15 (module-name import gating can't distinguish a trusted
package's internals from guest code) moved to `OpenQuestions.md`'s Resolved section.
Host-side dual-registry + C-level frame-stack-walk mechanism accepted, narrowing
ADR-002's numpy/pandas scope-limitation for the six gated names
(`ctypes`/`_ctypes`/`winreg`/`_wmi`/`_winapi`/`subprocess`). Not airtight — ADR-003
§9.2/§9.3 name residual risks explicitly (gadget-chaining, C-reentrancy, B12
registry-pointer-reuse INCONCLUSIVE) and flag a demonstrated pattern: three
independent misses (design, red-team, initial prove pass) each found a different
unhandled CPython import-machinery entry point.

**Open, unanswered thread:** user asked where CPython comes from and whether it
can be vendored from source instead of depending on the local Miniconda install,
since the project doesn't support `pip install` at all. This maps to
`docs/architecture/Python_Goals.md` Goal 3 / RFC 010 §10 Q1 (already an open,
undecided question in the spec, not something I invented). I offered to open it as
a proper design question (possibly a short ADR) — user has not yet responded.

**Why this matters:** the project is spec-driven (`CLAUDE.md`) — contested/hot-path/
security-critical designs need the full ADR cycle, not an ad-hoc decision. Don't
decide the CPython-sourcing question informally; if the user says go, open it
properly (design → maybe red-team if it's judged security-relevant → prove → judge),
matching the pattern already used for ADR-001/002/003.

**How to apply:** Next time this project comes up, check whether the user has
answered the CPython-sourcing offer. If yes, start that design work. If no, don't
assume — ask or wait, since it was left as an open offer, not a decision.
