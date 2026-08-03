# 024 — Versioning, Compatibility and Governance

**Status:** Draft · **Depends on:** all · **Gate:** §6

## Goal

Define how this project changes: what versions mean, what breaks and when, how a design decision
becomes binding, and how the RFCs themselves are amended.

## 1. What is versioned

| Artifact | Scheme | Breaking means |
|---|---|---|
| **Engine** | SemVer | Source or ABI break for embedders |
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

- **Q1** — Licence. Quark is MIT; matching it is the default assumption and is not yet decided by
  the project owner.
- **Q2** — Whether AgentEngine and Quark should share a release cadence, given the submodule
  coupling. (Previously framed around AgentEngine driving a macOS PAL requirement into Quark; 021 §7
  Q1 resolved macOS out of scope entirely, so this question now turns only on Windows/Linux PAL
  needs, both already met upstream.)
- **Q3** — Governance if the project takes outside contributions: who judges an ADR, and what
  quorum promotes an RFC.
- **Q4** — Security disclosure process and a `SECURITY.md` — required before any public release,
  given the project's threat surface.
