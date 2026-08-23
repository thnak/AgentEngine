# Design draft — does AgentEngine reverse 008 §1's "No `microvm` profile" locked decision?

**Status:** Revision 1 (design phase, 2026-08-23). Not yet red-teamed. No code written.
**Branch:** `microvm-first-party-backend`. **Depends on:** `decisions/ADR-080-sandbox-backend-registry.md`
(Proposed — the registry this design would register against).

## 0. Why this file exists

CLAUDE.md lists **"No `microvm` sandbox profile"** as a locked decision — "do not relitigate without
an ADR." The project owner has explicitly asked to open exactly that ADR. Per CLAUDE.md's own
discipline ("Contested... designs go through design → red-team → prove → judge, not an ad-hoc
change"), this is about as contested as a decision gets in this repo: it is not a gap or an
oversight, it is a decision 008 §1 already made, in writing, with a stated rationale. This draft's
job is to test that rationale honestly, not to manufacture a case for the outcome the project owner
signaled interest in. **A legitimate outcome of this design phase is "the locked decision stands, no
ADR ships a reversal, Design C wins."** That is not a failure of process — it is what red-teaming a
locked decision is for.

## 1. What 008 §1 actually says, and what would have to be wrong for it to change

Quoting the current text in full (008 §1, "No `microvm` profile"):

> An earlier draft of this RFC offered `microvm` (Firecracker/Hyperlight-class hardware isolation) as
> the strongest local boundary for hostile or multi-tenant workloads. It is dropped. The original
> rationale cited a missing macOS backend; that framing is superseded now that macOS is out of scope
> entirely (021 §7 OQ-1) — the real reason is sturdier: Firecracker is architecturally Linux+KVM-only
> ... so there is **no production-grade path to run it on Windows either**, the one platform besides
> Linux this project targets. Porting it would mean writing a new, unaudited VMM, not retargeting a
> build — exactly the second local isolation technology this decision declines to build and
> cross-platform-maintain ... A workload that genuinely needs hardware-level isolation is better
> served by the `remote` profile against infrastructure that already does this well ... than by
> AgentEngine building one of its own.

And 008 §1's priority statement, immediately after:

> Anything stronger and Linux-only — gVisor, Kata, a future microVM backend — is deliberately **not**
> something the engine commits to building or maintaining first; §2a makes explicit that the
> `SandboxBackend` seam is open precisely so a deployer who needs one can supply it without waiting on
> this project's roadmap.

Two distinct claims are bundled here, and they don't fall together:

- **Claim W (Windows-impossibility):** there is no production-grade microVM story on Windows, so a
  `microvm` profile could never be the cross-platform "one isolation contract" 008's own goal
  statement promises.
- **Claim M (maintenance-priority):** even Linux-only, the engine declines to be the one building and
  maintaining a hardware-isolation backend *first*, preferring to let the already-real `SandboxBackend`
  seam (008 §2a, now backed by a real registry — ADR-080) absorb that need from whoever wants it.

**Claim W is not in dispute and this ADR does not attempt to overturn it** — re-verified today,
unchanged (`docs/research/2026-08-23-microvm-sandbox-backend-landscape.md` §1: Firecracker, Cloud
Hypervisor, and the one unofficial Hyper-V fork are all still Windows-incapable or dormant). Any
design here is Linux-only by construction, same as `LinuxNativeJailBackend` already is.

**Claim M is the entire question this ADR has to answer**, and it is a resourcing/roadmap-priority
claim, not a technical-impossibility claim. Reversing it requires showing the maintenance cost is
bounded and worth paying now — not just that the registry now exists to register something against.

## 2. What's actually different since 008 §1 was written (the honest "why now")

- `decisions/ADR-080-sandbox-backend-registry.md` (Proposed) makes `SandboxBackend` a real,
  runtime-resolvable extension point (`register_backend`, `resolve_strict`/`resolve_named`) instead of
  a compile-time-only template parameter. Before ADR-080, *nothing* — first-party or consumer-supplied
  — could become the winner of `Strict` resolution; only `SandboxProfile<P>` naming a concrete type
  worked. This removes one real blocker to a first-party backend mattering at runtime, but it equally
  removes the blocker for a *consumer-supplied* one (Design C's whole premise) — it is not evidence
  specifically for first-party.
- Today's research (`docs/research/2026-08-23-microvm-sandbox-backend-landscape.md`) re-confirmed
  Claim W unchanged, and additionally found: **no competitor surveyed (MAF, E2B, Modal, Daytona,
  Cloudflare, OpenAI, Anthropic) exposes a genuine swap-the-isolation-technology seam** — every one
  ships a fixed backend (or small first-party menu) and, at most, lets a deployer relocate
  *infrastructure* for that same fixed backend. `SandboxBackend` is already ahead of the field on the
  *seam* axis. This is real, new evidence — but it argues for **advertising and documenting the seam**
  (Design C/D), not for the engine also becoming a first-party isolation-technology vendor; nothing in
  the survey shows demand specifically for AgentEngine-branded Kata/Firecracker integration.
- The same research also surfaced a **new, current cost data point against Design A/B**:
  `CVE-2026-1386`, an active 2026 privilege-escalation bug in Firecracker's own `jailer` privileged
  setup path — concrete evidence that "the most production-proven microVM stack" still has live,
  serious CVEs in exactly the code a first-party integrator would be responsible for keeping current.

**Net: nothing found today overturns Claim M's core worry (open-ended security-maintenance burden).**
If anything, the CVE finding sharpens it. The case for reversal has to rest on the value of shipping a
first-party option being worth that cost — this draft states that case as honestly as it can in §3
Design A, then lets red-team attack it.

## 3. Competing designs (steelmanned)

### Design A — first-party Linux-only backend wrapping Kata Containers, opt-in, `named_only` by default

A new `KataBackend` (Linux build only, `#if defined(__linux__)`, mirroring `LinuxNativeJailBackend`'s
existing platform split) satisfying `SandboxBackend`, shipped in the engine tree. Two safety rails
borrowed from precedent already in this codebase:

- **Gated like `ADR-071`'s `NativePythonProvider`**: a distinct, explicitly-scoped, host-opt-in
  capability — never enabled by default, never silently widening what `Strict` resolves to. Concretely:
  registered with `strict_eligibility::named_only` (ADR-080's own mechanism) unless a host explicitly
  re-registers it as `eligible` — so an existing deployment's `Strict` behavior is provably unchanged
  by this backend merely being *linked in*.
- **A build-time feature flag** (`AGENTENGINE_BUILD_KATA_BACKEND`, default `OFF`, mirroring
  `AGENTENGINE_BUILD_PYTHON_RUNNER`'s existing pattern) so a host that never wants the KVM/Kata runtime
  dependency doesn't pay for it — no new required toolchain for the v1 completeness target
  (`native-jail`).

Kata over Firecracker-direct: full VM boundary via KVM, `kata-clh`/`kata-qemu` runtime-class choice,
GPU passthrough (one GPU per pod), and critically — no direct dependency on the exact jailer code path
`CVE-2026-1386` lives in (Kata's own privileged setup is a different, more mature-tooled surface, per
the research's own comparison).

**Pro:** closes the "batteries-included hardened isolation" gap none of the seven surveyed competitors
close either; AgentEngine would be simultaneously the only platform with a swappable seam *and* one
with a first-party hardware-isolation option, which the research explicitly frames as a real
differentiator opportunity (§2 of the research doc).

**Con (the load-bearing objection, unresolved by anything in §2):** this is still the engine taking on
indefinite security-maintenance liability for a hypervisor-adjacent integration — CVE monitoring, KVM
version-matrix testing, the exact "second local isolation technology to build and cross-platform-
maintain" 008 §1 already declined, just narrowed to one platform instead of zero. It also breaks 008's
own stated v1 priority order ("native-jail first... anything stronger and Linux-only... is deliberately
not something the engine commits to building or maintaining first") without that priority itself
having changed — `native-jail`'s Linux backend and the interpreter-mediation defense-in-depth work
(§1b) are not stated as complete anywhere reviewed for this draft, which would make Design A
resourcing ahead of already-committed, not-yet-finished v1 work.

### Design B — first-party backend wrapping Firecracker + `jailer` directly

Same shape as Design A, strongest isolation and lowest cold-start on paper (≤125ms, per the existing
2026 standards-landscape record §8), but real, current operational costs the research reconfirmed
today: manual TAP/iptables/routing (no CNI-plugin-equivalent maturity), startup-latency degradation at
scale (up to 263% around ~400 concurrent starts per one surveyed source), and — the sharpest new data
point — the active, unpatched-as-surveyed `CVE-2026-1386` sitting directly in the jailer's privileged
directory-init path a first-party integrator would run. **Weaker safe-to-ship-as-default case than
Design A**, kept here only as the steelmanned alternative Design A was chosen over, not as a live
contender.

### Design C — no reversal; formalize the consumer-supplied path instead (the null hypothesis)

Ship nothing first-party. Write the "supplying a custom `SandboxBackend`" how-to guide the research
explicitly flagged as the real gap ("the mechanism is real, the worked example for a third-party isn't
written" — research doc §3, echoing the 2026-08-22 component-role-audit tracker's Finding C pattern),
with a real, buildable example wrapping one Linux technology (Kata or gVisor) living in `examples/` —
**not linked into the shipped `agentengine` library**, so it carries zero ongoing CVE-response
obligation for the core engine team; it is documentation-with-working-code, explicitly unmaintained-to-
production-SLA, the same posture Design D below formalizes. This is what "the locked decision stands"
looks like in practice — it is not "do nothing," it is "close the documentation gap that's real
without taking on the maintenance liability that isn't justified."

### Design D — partial reversal: ship a first-party *example*, not a first-party *product*

A middle position worth surfacing before red-team rather than after: Design C's `examples/` artifact,
but explicitly authored and reasonably maintained (compiles in CI, has tests) — just never advertised
as a supported `SandboxProfile` a production deployment should pick without forking it, and never
registered by anything the engine does automatically. The honest question for red-team: **is this
actually a reversal of 008 §1 at all**, or does it already fit inside "the `SandboxBackend` seam is
open... so a deployer who needs one can supply it" — i.e., can most of Design A's differentiation
value (a concrete, copy-from-here integration exists) be captured without touching the locked decision
or opening this ADR in the first place? If red-team confirms Design D doesn't require reversing
anything, that is itself a finding worth landing on: **this ADR may conclude "no locked-decision
change needed; ship Design D under 008 §2a as already-permitted,"** rather than either accepting or
rejecting a reversal.

## 4. Falsifiable claims (for red-team)

- **C1 (Windows-blast-radius-zero):** Design A/B introduce *zero* behavior change for any
  Windows-only deployment — `KataBackend` doesn't exist in a Windows build, and `strict_eligibility::named_only`
  means even a Linux host with it linked in sees no `Strict`-resolution change unless it explicitly
  opts in. *Disprovable if:* any code path lets the backend affect `Strict` resolution, or Windows
  build output, without an explicit host action.
- **C2 (bounded maintenance surface):** the ongoing security-maintenance liability Design A takes on
  is scopable to something concrete (e.g., "track Kata's own CVE feed, re-certify against N Kata LTS
  versions") rather than open-ended. *Disprovable if:* red-team shows the liability is unscopable —
  e.g., transitive CVEs in QEMU/cloud-hypervisor that Kata itself doesn't fully insulate, or that "one
  GPU per pod" / runtime-class selection multiplies the support matrix unpredictably.
- **C3 (I2/I3 unaffected):** registering a hardware-isolation backend introduces no new path for model
  output to influence which backend a `Strict`-resolved agent gets, and no new ambient authority.
  *Disprovable if:* any resolution path lets tool-call or model-generated data reach `resolve_named()`
  or backend selection without going through `HostSandboxSelection`'s existing explicit-construction
  bar (ADR-080 §2).
- **C4 (real demand, not assumed):** there is a genuine, sourceable demand signal for AgentEngine
  itself shipping this, distinct from "the seam should exist" (already true) or "an example should
  exist" (Design C/D). **This is the weakest claim in the draft and is expected to be judged
  INCONCLUSIVE or WRONG** absent a project-owner-supplied concrete use case — the research this draft
  cites found demand evidence for the *seam*, not for first-party ownership of one specific backend.
- **C5 (v1-priority-consistent):** taking on Design A/B's work does not come at the expense of 008's
  own stated v1 completeness target (`native-jail` across both platforms, §1b's interpreter-mediation
  defense-in-depth). *Disprovable if:* `native-jail`'s own v1 gate (008 §9) is not yet met — in which
  case Design A/B would be prioritizing new scope over already-committed, unfinished scope.

## 5. What red-team should specifically try to break

1. **Security/I2-I3 lens:** attack C1 and C3 directly — find any path where a hardware-isolation
   backend's mere presence (linked, registered `named_only`) changes observable behavior for a host
   that never opted in; find any way model/tool-call output could reach backend selection.
2. **Maintenance-economics lens:** attack C2 — is "track Kata's CVE feed" actually bounded, or does
   Kata's own dependency chain (QEMU, cloud-hypervisor, containerd shims) make this an unbounded
   commitment in practice, the way `CVE-2026-1386` was a surprise in the *simpler* Firecracker jailer?
3. **Architecture-fit / precedent lens:** attack whether Design A genuinely needs a locked-decision
   reversal at all (i.e., stress-test Design D's "maybe this needs no ADR" finding), and whether v1
   priority (C5) is actually being honored — pull the real current status of 008 §9's gate before
   accepting C5.
4. **Steelman the reversal itself:** a fourth lens should argue *for* Design A as hard as possible —
   the case that shipping one well-chosen, well-gated, opt-in backend is a legitimate product decision
   distinguishing AgentEngine from every competitor surveyed, and that "we decline to be the first to
   build this" was a reasonable v1-scoping call in 008's original context (pre-registry, pre this
   research) that a real registry and this evidence now justify revisiting — not to manufacture
   consensus, but so the design that survives red-team survived a real fight, not an uncontested one.

## 6. Open decision for red-team/judge, not resolved here

Whether the honest outcome of this process is **Design A** (reversal, real first-party backend,
opt-in and blast-radius-contained), **Design D** (no reversal needed — ship an example under the
already-permitted seam), or **Design C** (no reversal, document-only) is explicitly left open. This
draft's own reading of the evidence in §2 leans toward Design C/D being better supported by what
changed today than Design A/B — but that is a design-phase impression, not a conclusion, and red-team
gets the real vote.
