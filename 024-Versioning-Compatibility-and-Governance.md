# 024 — Versioning, Compatibility and Governance

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** all · **Gate:** §6

## Goal

Define how this project changes: what versions mean, what breaks and when, how a design decision
becomes binding, and how the RFCs themselves are amended.

## 1. What is versioned

| Artifact | Scheme | Breaking means |
|---|---|---|
| **Engine** | SemVer | Source break for embedders (020 §3a: source-level embedding, not binary-stable) |
| **C ABI** (020 §8 Q3) | SemVer, independent of Engine | Binary break for out-of-process bindings; frozen only after a reference consumer exists (OQ-8) |
| **Agent** | SemVer (002 §7) | Behaviour change: instructions, tools, output schema |
| **WIT worlds** (009) | SemVer per world | A plugin compiled for it no longer loads |
| **Declarative format** (015) | `apiVersion` | A document no longer loads or means something different |
| **Wire/persistence formats** | Format version + migrations | An old checkpoint no longer reads |
| **Protocol conformance** | The upstream revision string, verbatim | We move revisions |
| **RFCs** | Revision number + status ladder | A normative statement changes |

## 2. Compatibility promises

- **Engine ≥ 1.0**: no source break within a major version. Deprecated APIs remain for at least one
  minor cycle with a compile-time deprecation.
- **Plugins**: the engine supports the current WIT world version and **N-1**, with a **12-month
  minimum** deprecation window — deliberately mirroring MCP's feature lifecycle policy, so a plugin
  author faces one deprecation cadence, not two.
- **Persistence**: checkpoints and sessions are readable across engine upgrades within a major
  version; a format change ships a forward migration, and a migration that can lose data requires
  an explicit operator acknowledgement.
- **Protocols**: we track upstream revisions and state which we implement. Supporting two revisions
  means two constants and two conformance suites (CONVENTIONS), never a runtime `if` that pretends
  to be both.
- **Pre-1.0**: everything may break, and the changelog says exactly what did.

## 3. Deprecation

Every deprecation carries: the reason, the replacement, the removal version, and the date it was
announced. The registry of currently-deprecated features lives in `DEPRECATED.md` — one place an
integrator can read to know what is going away, which is a pattern worth copying precisely because
MCP proved its absence hurts.

**Security exception:** a deprecation required to close a vulnerability may move faster than the
window, with the reason stated. Nothing else may.

## 4. The decision process

Inherited from Quark, because it works and because contributors move between the repos:

1. **RFC** — a design lands as a numbered spec (or an amendment to one) with explicit open
   questions. Status **Draft**.
2. **Design → red-team → prove → judge** — for contested, hot-path, or security-critical choices:
   competing designs are implemented in real code, attacked, measured, and judged on evidence.
   Produces an **ADR** in [`decisions/`](decisions/).
3. **Promotion** — the RFC's named gate is executed; status moves Draft → Reviewed → Proven →
   Accepted (platform).
4. **Code follows the spec.** When code and spec disagree, the spec wins; if the spec is wrong, fix
   the spec first, with the ADR that proves it.

**A design settled by argument alone does not reach Proven.** For security properties specifically,
a claim without a positive control (022 §5) does not count as proven at all.

## 5. RFC hygiene

- **Numbers are permanent.** A superseded RFC is marked Superseded and points forward; it is never
  renumbered and never deleted.
- **Amendments are dated**, and normative changes list what changed for implementers.
- **Every RFC carries** status, dependencies, a promotion gate, and open questions. An RFC with no
  gate cannot be promoted, which is the point.
- **Research is dated and cited** (`docs/research/`); a claim about the outside world names its
  source and its date, because the outside world moves (MCP moved twice in eight months).
- **Cross-references are maintained**: an invariant restated in another RFC names the owning RFC.

## 6. Promotion gate

- **G1** — a link/consistency checker validates every cross-reference, gate reference, and status
  claim across all RFCs in CI.
- **G2** — the invariant→test coverage map (022 §7 G3) is complete for every Accepted RFC.
- **G3** — a deprecation-registry test: every deprecated symbol/feature in code appears in
  `DEPRECATED.md` with a removal version, and vice versa.
- **G4** — a persistence-migration test reads fixtures from every prior format version.

## 7. Open questions

- ~~**Q1** — Licence. Quark is MIT; matching it is the default assumption and is not yet decided by
  the project owner.~~ **Resolved, MIT (OQ-11, 2026-08-04):** confirmed by the project owner,
  matching Quark exactly — avoids a licence mismatch between the engine and the submodule it depends
  on for everything (CLAUDE.md's locked decision that Quark is never forked, only depended on), and
  is the assumption this RFC was already carrying. `LICENSE` at the repo root.

  **Third-party dependency licensing position** (raised by `docs/planning/v1-office-user-toolkit.md`
  against Pandoc/GPL-2.0-or-later and `docxtpl`/LGPL): MIT places no copyleft obligation on the
  engine itself, and it places none on dependencies either, so the question is really about *how*
  each dependency is consumed. A GPL tool invoked as a separate process (Pandoc via subprocess, no
  static or dynamic linking into the engine binary) does not make the engine a derivative work under
  the FSF's own stated position on mere aggregation/process invocation. An LGPL library consumed via
  dynamic/import-time linking (a Python package like `docxtpl`, resolved at runtime rather than
  compiled in) is exactly the case LGPL was written to permit without imposing its terms on the
  linking application. Both are adoptable under this policy; a dependency that would require *static*
  linking of GPL/LGPL code into the engine binary is not, and needs a case-by-case legal read before
  adoption, not an assumption either way. This is a general policy for future dependency choices, not
  a review of every current one — `010 §10 Q1` (pinned-image CVE cadence) is the adjacent, still-open
  operational question for the same Python-package surface.
- ~~**Q2** — Whether AgentEngine and Quark should share a release cadence, given the submodule
  coupling.~~ **Resolved, No, independent cadences (2026-08-04, no longer entangled with a macOS PAL
  requirement — 021 §7 OQ-1 resolved macOS out of scope entirely):** the same deliberate-pin-bump
  discipline already established for every other pinned dependency this session (Wasmtime, the
  interpreter image, GenAI semconv) applies to the Quark submodule pin too, and that pattern doesn't
  need a synchronized release cadence to work — AgentEngine bumps its Quark pin on its own schedule,
  gated on its own full suite passing against the new commit, never automatically tracking Quark's
  release dates. Sharing a cadence would add cross-project release-train coordination for no benefit
  this already-working pattern doesn't deliver. The two projects sharing contributors (§4's stated
  reason for sharing a *decision process*) is a different, weaker coupling than sharing a *release
  schedule*, and only the former is actually needed.
- ~~**Q3** — Governance if the project takes outside contributions: who judges an ADR, and what
  quorum promotes an RFC.~~ **Resolved for the current phase, deferred by construction for the
  question it's actually asking (OQ-11, 2026-08-04):** there is no remote and no outside contributor
  today (git state: local repository only) — §4's judge role is held by the project owner alone, the
  same way Quark's own repo has no separate governance document either (checked: none exists). A
  quorum rule is meaningless with one contributor, so it is not invented speculatively; it is owed
  once the project actually takes outside contributions, at which point it is new design work done
  at that time — matching Quark's if Quark has settled one by then (§4's "contributors move between
  the repos" reasoning), a fresh minimal default (e.g. two-maintainer approval) otherwise. What *is*
  resolved now: the judge is unambiguous while solo, so nothing about promoting an RFC or landing an
  ADR is blocked by this question in the meantime. **Extended (2026-08-05):**
  `docs/planning/v1-review-signoff-workflow.md` §3 designs the mechanism for once collaborators
  exist — judge-must-not-be-author separation-of-duties, not a fixed quorum number, which stays
  exactly as open as this entry left it.
- ~~**Q4** — Security disclosure process and a `SECURITY.md` — required before any public release,
  given the project's threat surface.~~ **Resolved (OQ-11, 2026-08-04):** `SECURITY.md` written at
  the repo root — private reporting channel (placeholder contact until a public remote exists),
  acknowledgement/disclosure timeline, the §3 security-exception deprecation carve-out cross-
  referenced, and the corpus publication split (classification public, payloads private) that OQ-10
  resolved for 017 §9 Q4 / 022 §8 Q2. Written now, ahead of any public release, so it exists before
  it's needed rather than being drafted under incident pressure.
