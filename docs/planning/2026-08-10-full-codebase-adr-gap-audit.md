# Full-codebase ADR-conformance and gap-closure audit

**Run:** 2026-08-10, `wf_6d9637fb-786`, 130 agents (~9.5M tokens, ~41 min) · **Status:** audit output,
not itself a spec or ADR — every finding below still needs its own ADR before anything is built
against it, per the process this document's own findings confirm the project actually follows.

**Update (2026-08-13, status only — the body below is unedited, per this doc's own "verbatim...
unedited except this header" convention):** two real-code events since this run change two rows'
status. Neither was re-derived from scratch here — both are cross-checked against the actual ADRs
that landed, matching this doc's own evidence discipline.

**Update (2026-08-14, status only, same convention):** five real-code events and one design-only
(no-code) event.

- **Gap 2 (AgentMetadata/Workflow missing description/version) is CLOSED.**
  `decisions/ADR-044-agent-workflow-description-version.md` (Proposed) confirms this is not new
  surface — 002 §1/§7 has specified `{agent_id, description, version}` as agent identity since the
  RFC was written; three independent compilers (A2A Agent Card, Agent YAML, Workflow YAML) hit the
  identical gap independently and each honestly recorded it. Fixed via `requires`-detected optional
  statics (not required — avoids breaking every existing native agent), 4 real call sites wired (the
  audit's "5" was an overcount), and a checked-not-assumed field-insertion-order risk (grepped for
  positional aggregate-init before landing fields mid-struct, per `Usage`'s own prior amendment
  lesson). I6 test extended, full suite green.

- **Gap 15 (ack races ahead of durability) is CLOSED.** `decisions/ADR-043-ack-durability-policy.md`
  (Proposed) corrects this row's own cited blocker on both counts: a `SessionStore` seam already
  existed and was already wired into `save_agent_session_snapshot()`/`checkpoint_if_due()` (it was
  never meant to be type-erased — a deliberate `concept`, matching this project's `ChatClient`/
  `SecretStore` precedent), and 005 §2 already specifies the exact policy vocabulary
  (`at_most_once_ack`) rather than needing the audit's invented `AckPolicy`/`AtMostOnceAck`/
  `RequireDurableAck` names. Fixed via two new free functions
  (`start_run_with_ack_policy()`/`resolve_interaction_with_ack_policy()`) that sequence Store writes
  around the existing entry points without touching `AgentSession`'s own internals at all — sidesteps
  this row's own "insertion point breaks a tested invariant" concern entirely, since nothing is
  inserted into `run_rounds()`. Real bonus finding: 005 §2 says "history delta," not full history —
  reusing `rt/message_codec.hpp`'s already-proven codec makes genuine turn-content durability
  (not just bookkeeping) tractable now, closing more of 005 §2 than the audit's own framing assumed
  was reachable. Also surfaced and fixed a real, latent ADL ambiguity in `message_codec.hpp` itself.
  Self-red-team named one real, unfixed residual: delta capture isn't race-free against concurrent
  overlapping `start_run()` calls on the same session (only sequential, single-caller usage — the
  dominant case 005 §2's own gate describes). New test file, full suite green.

- **Gaps 16 and 21 (the I3 taint pair) are PARTIALLY CLOSED, together, as this row's own note said
  they had to be.** `decisions/ADR-042-context-instructions-taint-channel.md` (Proposed) confirms the
  drop site is one level later than gap 16's framing implied (`assemble_context()` reads
  `.instructions` correctly; `rt::AgentSession::run_rounds()` is where it actually vanishes, and
  `ChatRequest` had no slot for it), retypes `ContextContribution::instructions` to `TaintedText` —
  giving `Tainted<T>` (Judged as a type, 007 §9 G2, but never previously instantiated as a real field
  anywhere) its first real content-model use — and wires the one explicit declassification site into
  `run_rounds()`. Gap 16 is CLOSED for real: instructions now reach the model as a genuine
  `role::system` message, proven by two new/extended test files (176/176 full suite). **Gap 21 is only
  PARTIALLY closed** — this ADR deliberately gives `Tainted<T>` a real field only where gap 16 needed
  it, not the full content-model migration gap 21's broader claim describes; `ContentItem::text` and
  the three originally-cited consumption boundaries (`cli_chat.cpp`/`mcp/server.hpp`/`tool_bridge.hpp`)
  remain raw and ungated, named as a real, still-open residual, not silently claimed closed.

- **Gap 10 (Linux native-jail zero FS/process containment) is DOCUMENTED, not closed — deliberately,
  matching this pass's own project-owner-set precedent for OQ-19/OQ-20.**
  `docs/planning/linux-native-jail-pivot-root-containment-design-draft.md` re-confirms this row's
  current-state claim is accurate (zero containment, not stale), corrects the row's "blanket `/usr`
  bind-mount" claim (nothing in this codebase bind-mounts `/usr`, or ever proposed to — dropped, not
  fixed), designs a `pivot_root`+bind-mount+fresh-`/proc` mechanism reusing the already-proven Linux
  path-validation primitive, and self-red-teams it: MUST-FIX 1 (mount-namespace propagation must be
  set `MS_PRIVATE` before any bind mount, or bind mounts leak into the host's own mount table — a
  real, easy-to-miss ordering invariant, not a hypothetical) and MUST-FIX 2 (a read-only bind mount
  needs a required second `MS_REMOUNT` call; the initial `MS_BIND` call silently ignores `MS_RDONLY`).
  **Not implemented or built**: this session's machine is Windows-only, with no Linux dev/test
  environment to compile or execute against — the design draft is explicit that a future Linux
  environment is required before any of this becomes real, tested code, the same gap this row named
  is still open, now with a design ready rather than a blank page.
- **Gap 11 (Windows AppContainer ACE leak) is CLOSED, and its own recommended approach is corrected,
  not followed.** `decisions/ADR-041-appcontainer-ace-leak-accepted-residual.md` (Proposed) finds this
  row's cited blocker (`CreateProcessAsUser`/`CreateProcessWithTokenW` needing privilege) is a
  misattribution — that API is used nowhere in this codebase and was never proposed in ADR-004; the
  real launch mechanism already runs under a standard non-admin token. The real, ADR-004-evidenced
  obstacle (writing a DENY ACE onto a `SYSTEM`-owned file fails the same way granting an ACE on a
  system-owned install already failed, per ADR-004 §5.4) blocks the deny-SID approach for a different
  reason than this row named. No new mitigation ships: the accepted fix (interpreter-level `open()`
  mediation as the PRIMARY filesystem boundary, 008 §1b, already Judged and shipped) already means a
  guest request for the leaking files never reaches the leaky ACL check. What this row actually asked
  for — proof the leak is real but bounded — already existed and already passes
  (`tests/test_native_jail_abuse_corpus_windows.cpp` Case 4, predating this pass); ADR-004 §11 item 1
  asked for exactly this test and had gone stale (never marked done), corrected in the same pass.
- **Gap 12 (ShellRunner write-quota false denial) is CLOSED, and its own deferred FsRead note is
  folded in and CLOSED too.** `decisions/ADR-040-fs-quota-capability-gate-fix.md` (Proposed) confirms
  the false-denial bug re-verified against current code (the audit's "ShellRunner" is precisely
  `mediated_shell_dispatch.cpp`, not the untouched `shell_dispatch.cpp` spike), ships the gate fix
  paired with live usage enforcement exactly as this row's own recommendation required, and
  additionally closes gap 3/12's deferred "a symmetric FsRead/size_cap_bytes capped-grant gate bug...
  was not in scope for this pass" note rather than leaving it a second open follow-up. New regression
  block (`test_mediated_shell_runner_smoke.cpp`, E3-Q0-Q5) proves both the false-denial fix and the
  live-enforcement fix against a real mount with a byte-exact-known baseline; full suite 175/175
  green. One half named, not closed: the identical fix applied to `mediated_python_runner.cpp`'s
  `Internal_open`/`Internal_listdir` read branches is a mechanical pattern-match, not locally
  build/test-verified this session (`AGENTENGINE_BUILD_PYTHON_RUNNER=OFF` in this environment).

- **Gap 8 (naming-lint namespace bug) is CLOSED.** Its suggested title,
  `naming-lint-namespace-coverage-and-vocabulary-reconciliation`, matches
  `decisions/ADR-025-naming-lint-namespace-scope-and-027-vocabulary-diagram.md` (Judged) — confirmed
  the exact bug this row named (`tools/naming_lint.py` matching the literal string `"agentengine"`
  only, blind to `trust::`/`sandbox::`/`workflow::`) and fixed the scanner. That ADR's own text is
  explicit that the ~263-name BULK RECONCILIATION against 027 §2-4 is deliberately NOT done in the
  same change (sequencing decision, not an oversight) — so "gap 8 closed" means the scanner is fixed
  and honestly failing against the real surface, not that every unlisted name has been triaged yet.
- **Gap 23 (milestone-status doc drift) is CLOSED.** Its own suggested title,
  `milestone-status-doc-accuracy-and-drift-lint`, is exactly what shipped as
  `decisions/ADR-026-milestone-status-doc-accuracy-and-drift-lint.md` (Judged) — the CLAUDE.md/
  README.md/marketing-site corrections and `tools/milestone_status_lint.py` this row called for are
  real. A separate, unrelated pass (2026-08-13, the post-ADR-037 Quark-mention sweep) additionally
  corrected residual staleness this row didn't originally scope (RFC-body present-tense Quark claims,
  code-comment banners) — see `decisions/README.md`'s own ADR-037 row for that sweep's own accounting.
- **Gap 1 (network listener) is REDIRECTED, not closed — and its own recommended approach is now
  explicitly rejected.** `decisions/ADR-039-inbound-transport-host-pluggable.md` (Proposed,
  2026-08-13) supersedes ADR-021 §3–8/ADR-022 in full: it does NOT "build the already-Judged
  listener/TLS/parser now" (this row's own recommendation) — it accepts that AgentEngine builds no
  first-party listener at all, ever, matching MAF/OpenAI Agents SDK precedent neither this audit nor
  ADR-021/022 had checked against. Two things this row named ARE independently addressed inside
  ADR-039: principal propagation (closed via a real, proven `trust::principal_from_bearer_claims()`
  bridge plus a session-scoped, not per-call, `Principal` binding contract) and the same **A2A
  cross-tenant task-history leak** this audit's own transcript found — ADR-039 §3d independently
  re-derives it from current code (`a2a/server.hpp`'s own comment: "no principal/authorization
  boundary in this transport-agnostic dispatcher yet") and names it as an explicit, not-yet-closed
  residual, not a silently dropped one.
- **The other 14 gaps below were NOT re-verified this pass** (gap 10 above was re-verified and
  designed against, but stays open — not counted as re-verified-and-closed like 2/8/11/12/15/16/21/23;
  gap 21 itself is only partially closed, see its own row below). They still reflect the 2026-08-10
  snapshot. ADR-037 (Quark removal, 2026-08-13) touched large parts of the tree these gaps cite by
  file:line — the underlying LOGIC most of these findings describe (capability/taint/sandbox/memory
  behavior) is independent of the actor-engine substrate ADR-037 replaced, so the findings themselves
  are not expected to have been invalidated by it, but exact file:line citations in the full per-gap
  transcripts may no longer resolve to the same lines, or even the same file (e.g. anything under the
  old `core/agent_session.hpp`, deleted by ADR-037, now lives under `rt/agent_session.hpp`). Re-verify
  citations before acting on any of the remaining 21, don't assume they still resolve as written.

Produced by a 5-stage multi-agent workflow: 9 discovery agents swept the codebase by
milestone/RFC cluster plus a dedicated ADR-conformance sweep and a roadmap-claims-vs-reality
sweep, seeded with 7 gaps already known from the Milestone 7 Phase G audit
(`docs/planning/milestone-7-protocol-conformance-breakdown.md`); every new finding got 3
independent adversarial verifiers (killed if ≥2 voted refuted); every surviving gap went through
an architect → red-team → judge panel checking against I1-I8 and the locked decisions in
`CLAUDE.md`. Full per-gap architect/red-team/judge transcripts (far too large to commit — one gap
alone ran to tens of thousands of words) live in this session's local workflow transcript
directory (`journal.jsonl`, one `{"type":"result",...}` line per agent) if a future ADR author
needs the full reasoning behind a specific line item below, not just the one-line summary.

---

<!-- Report body verbatim from the workflow's synthesis agent, unedited except this header. -->

## Executive Summary

- **17 gaps discovered**, **23 gaps confirmed** through design → red-team verification (red-team surfaced additional, independently-confirmed problems while vetting the original set — several architect proposals were themselves found to introduce new invariant violations).
- **0 gaps implemented.** Every one of the 23 confirmed gaps carries a `needs_adr` verdict. No code shipped in this audit pass — the review process concluded that closing any of these gaps directly, as first proposed, would either leave the underlying invariant broken, silently mislabel a still-broken thing as fixed, or introduce a new violation of comparable or greater severity.
- **Severity mix:** 8 high, 13 medium, 2 low.
- **Kind mix:** 9 `missing_definition` (undesigned or unbuilt), 5 `spec_drift` (code and RFC have quietly diverged), 5 `claim_mismatch` (a document asserts something the code doesn't do), 4 `adr_violation` (code contradicts an already-Judged ADR).
- **Headline pattern:** the single most common red-team finding, repeated across roughly a third of the confirmed gaps, is that the architect's first-pass fix would have converted a *safe, visible* failure (a spurious denial, a loud compile error, an honestly-failing CI gate) into an *unsafe, silent* one (a quota bypass, an unenforced authorization boundary, a green checkmark over unaudited code). This is worth flagging to the reader up front: the audit's greatest value this round was in catching that pattern before it shipped, not in closing gaps.
- **Two of the highest-severity findings are meta-findings about the audit trail itself**: the naming-lint gate (027) that's supposed to prove naming conformance has a namespace-matching bug that makes it blind to the codebase's most security-relevant modules (`trust/`, `sandbox/`, `workflow/`), and the project's own status documents (CLAUDE.md, README.md, and the marketing site) currently claim Milestone 7 "has not started" when it is substantially built.
- Three findings concern the sandbox/native-jail boundary directly (Linux FS/process containment, Windows AppContainer ACE leakage, ShellRunner quota enforcement) — these are the highest-consequence items for I2 ("no ambient authority") and deserve first attention.

---

## Prioritized Gap Table

| # | Title | Severity | Verdict | Recommended approach (one line) |
|---|---|---|---|---|
| 1 | No real network listener despite ADR-021/022 deciding its shape | **High** | ~~needs_adr~~ **[2026-08-13: REDIRECTED, ADR-039 Proposed]** | ~~Build the already-Judged listener/TLS/parser now~~ — superseded: ADR-039 rejects building one at all; principal propagation and the A2A cross-tenant leak this row named are both independently addressed there (see header update). |
| 8 | 027's naming-lint gate fails on main (76 unlisted + 112 suppressed names) | **High** | ~~needs_adr~~ **[2026-08-13: CLOSED, ADR-025 Judged]** | Scanner fixed as recommended; bulk table reconciliation against 027 §2-4 deliberately deferred by that same ADR, still open. |
| 10 | Linux native-jail gives the guest full host FS read/write + process visibility | **High** | needs_adr **[2026-08-14: DESIGNED, not implemented — see planning doc below]** | ~~Build pivot_root+bind-mount containment... narrow the blanket `/usr` bind-mount.~~ — the `/usr` bind-mount claim didn't correspond to real code; `docs/planning/linux-native-jail-pivot-root-containment-design-draft.md` reuses the accepted primitive as recommended, and its own red-team found two MUST-FIX ordering bugs (`MS_PRIVATE` propagation, two-step read-only remount) a naive implementation would have missed. Not built: no Linux dev/test environment available this pass. |
| 12 | ShellRunner write builtins unconditionally deny writes under any quota-capped FsWrite grant | **High** | ~~needs_adr~~ **[2026-08-14: CLOSED, ADR-040 Proposed]** | Gate fix + live usage enforcement shipped together as recommended, plus the deferred FsRead mirror bug (gap 3/12's own note) folded in and closed too; Python-runner half is a pattern-match fix not locally build-verified (`AGENTENGINE_BUILD_PYTHON_RUNNER=OFF` here). |
| 15 | AgentSession acks a turn before it's durable; no `at_most_once_ack` escape hatch | **High** | ~~needs_adr~~ **[2026-08-14: CLOSED, ADR-043 Proposed]** | ~~Add an AckPolicy... the proposed insertion point sits after run_finished is already emitted... the required type-erased Store seam doesn't exist yet.~~ — both premises wrong: the Store seam already existed (a deliberate `concept`, never meant to be type-erased) and the fix never touches `run_finished`'s emission at all, since it's enforced entirely in two new free functions around `AgentSession`, not inside it. |
| 16 | `ContextContribution.instructions` computed but silently dropped before reaching the model | **High** | ~~needs_adr~~ **[2026-08-14: CLOSED, ADR-042 Proposed]** | Routed through `role::system` as recommended, gated by making `Tainted<T>` real for this one field first (closing the "must close that first" precondition this row itself named) rather than the whole content model. |
| 19 | Image/Audio/Video/File (and Anthropic Reasoning) content silently dropped outbound despite declared capability bits | **High** | needs_adr | Split into Phase 1 (fail-closed capability gate + symmetric drop-signal, implementable now) and Phase 2 (real wire encoding), which is blocked on RFC 019's blob-store seam — which does not exist anywhere in the tree. |
| 23 | CLAUDE.md/README claim Milestones 7-9 "have not started" though M7 is substantially built | **High** | ~~needs_adr~~ **[2026-08-13: CLOSED, ADR-026 Judged]** | Docs corrected, lint (`tools/milestone_status_lint.py`) shipped and wired into CI as recommended. |
| 2 | AgentMetadata/Workflow have no description/version fields | Medium | ~~needs_adr~~ **[2026-08-14: CLOSED, ADR-044 Proposed]** | Optional fields wired through 4 real call sites (the "5" was an overcount); the `Tool<>` naming overlap is real but not a compile collision (unrelated CRTP bases) — named explicitly rather than "closed"; I6 equivalence guarantee stated honestly as before (still example-proven, not structural). |
| 3 | No generic JSON Schema 2020-12 validator | Medium | needs_adr | Build `core/json_schema_validator.hpp` with the proposed keyword subset/budgets — but wire it at `invoke_tool()`'s InvokeFn construction site (not the MCP dispatcher), add the missing outbound client-side strict check, and close a regex-DoS/`$ref`-cycle budget gap. |
| 4 | No tool/capability name-keyed registry | Medium | needs_adr | The submitted proposal was placeholder text with no real content — a real design must resolve I2 capability-ceiling binding, I3 resolution timing, namespace squatting, and WASM-ABI conformance from scratch. |
| 5 | ToolTable has zero runtime construction API (name-keyed half) | Medium | needs_adr | Add a `ToolRegistry` wired through `compile_agent_document` — but the proposed nullptr-fail-closed policy inverts an existing convention and breaks a currently-passing test (015 §2's own worked example). |
| 7 | M7 Phase G gate 006 §6b G6 (`schedule_wakeup`) is unprovable as written | Medium | needs_adr | Give `schedule_wakeup` a real `StandingEffect` producer — but must enforce `Schedule<max_horizon>` at arm time (currently unbounded, a live I2 gap) and name the missing `ReminderService`-access seam. |
| 9 | 027's canonical name `UsageDetails` has drifted to `Usage` in code | Medium | needs_adr | A mechanical rename only works once 003 §6 and 004 (which normatively say `Usage`, not `UsageDetails`) are reconciled too — otherwise it trades one spec/code drift for another. |
| 11 | Windows native-jail leaks curated host files via inherited AppContainer ACEs | Medium | ~~needs_adr~~ **[2026-08-14: CLOSED, ADR-041 Proposed]** | ~~The deny-only-SID fix is blocked pending an empirical spike: the proposed launch APIs (`CreateProcessAsUser`/`WithTokenW`) need privileges a standard non-admin deployment account likely doesn't hold.~~ — this blocker is a misattribution (that API isn't used anywhere in this codebase); the real fix (interpreter-level `open()` mediation as primary boundary) already shipped and is already Judged, and the leak's boundedness is already proven by an existing, passing test. |
| 13 | Shell-dispatched registered Tools bypass the 006 §3 tool pipeline | Medium | needs_adr | Route shell tool calls through `bridge_tool_call` like Python's bridge already does — but fix a null-`tool_bridge` crash, stop collapsing capability/approval denials into continuable errors, and give `call_index` a real source. |
| 17 | MemoryProvider renders ModelInferred and UserStated items identically | Medium | needs_adr | Add a confidence-label prefix — but must pair it with fixing Anthropic's zero-separator system-message concatenation (labels visually bleed together) and address a new label-forgery surface the fix itself introduces. |
| 18 | Default memory ranking is additive, not salience×recency×keyword | Medium | needs_adr | Fix the formula to a literal product (sound) — but the proposed recency signal is non-monotonic on overwrite and adds a real O(item-count) traversal; use the Store's actual sequence number instead. |
| 20 | Cross-provider Reasoning-exclusion design (003 §8 Q2) never implemented | Medium | needs_adr | Wire `ChatClientId` provenance through — but a real production call site (`HistoryAndSkillsProvider`) was missed, and `FailoverChatClient`'s Primary-only identity would falsely flag ordinary failover as cross-provider. |
| 21 | Content model never uses type-level `Tainted<T>` for text/structured fields | Medium | needs_adr **[2026-08-14: PARTIALLY CLOSED, ADR-042 Proposed — see gap 16]** | ~~Proposed accessors are additive and bypassable...~~ — ADR-042 gives `Tainted<T>` its first real field (`ContextContribution.instructions` only, needed by gap 16). The three named consumption boundaries (cli_chat.cpp, mcp/server.hpp, tool_bridge.hpp) still read raw `ContentItem` fields unchecked — this row's broader claim stays open, a full content-model migration is real, separately-scoped RFC-003-§2-level work. |
| 22 | 014 §8 G3 (10³-seed scheduling shuffle test) never built, undisclosed | Medium | needs_adr | Write the missing test per the M6 breakdown's own already-designed architecture — but "the test exists" is falsely claimed in more than the two spots the architect named; all must be retracted together. |
| 6 | `output_schema $ref` never resolved by the declarative compiler | Low | needs_adr **[2026-08-14: re-verified, still open — see priority note below]** | Add an opt-in `SchemaRefResolver` — but must extract the *full* enforceability check (not half), reuse ~~`worktree.hpp`'s~~ **`worktree_mount_fs.hpp`'s** (this row's own citation was wrong — `worktree.hpp` has no path-validation logic) tested path validator for Windows-safe traversal checks, and not overstate unbuilt digest/signing infrastructure as a safety net. |
| 14 | 025 §4 conflict-surfacing (`/conflicts/<path>.<agent>`) unimplemented | Low | needs_adr | Materializing evidence into the parent Ref via `commit_ref` directly contradicts an already-passing test ("parent Ref unchanged on failed merge") and misattributes ours/theirs — needs a different storage location and a real committer-identity source. |

---

## Implemented vs. Needs-ADR vs. Deferred

### What was implemented

**None.** The `implemented` list from this audit is empty — zero files changed, no build or test run to report. Every confirmed gap's verdict is `needs_adr`: in each case red-team found the architect's first-pass fix either left the invariant it targeted still broken, or introduced a new, comparably serious defect (most often a *silent* one replacing a *loud* one). Per this repo's own process (`design → red-team → prove → judge`, CLAUDE.md), none of these clear the bar for direct implementation.

### What needs a real ADR

All 23 confirmed gaps. Each carries a suggested title, but nearly all were independently suggested as `ADR-025-*` — **these cannot all be ADR-025**; sequential numbers must be assigned at write time. Grouped by subsystem for planning:

- **Trust boundary / network listener:** `ADR-025-inbound-principal-propagation-and-per-resource-authorization.md` (gap 1)
- **Agent/workflow metadata & declarative surface:** `agent-workflow-description-version-metadata` (2), `json-schema-instance-validator-wiring` (3), `name-keyed-tool-capability-registry` (4), `declarative-tool-registry-resolution` (5), `declarative-output-schema-ref-resolution` (6)
- **Workflow orchestration proofs:** `schedule-wakeup-standing-effect-and-horizon-enforcement` (7), `workflow-g3-shuffle-determinism-test` (22), `conflict-evidence-materialization` (14)
- **Governance / documentation integrity:** `naming-lint-namespace-coverage-and-vocabulary-reconciliation` (8), `usage-usagedetails-canonical-name-reconciliation` (9), `milestone-status-doc-accuracy-and-drift-lint` (23)
- **Sandbox / native-jail (highest consequence for I2):** `linux-native-jail-pivot-root-containment` (10), `windows-native-jail-appcontainer-token-hardening` (11), `shellrunner-fswrite-quota-gate-and-enforcement` (12), `shell-tool-dispatch-bridge-unification` (13)
- **Session durability & content taint (I3/I4):** `agent-session-ack-durability-policy` (15), `context-instructions-taint-channel` (16), `content-item-taint-enforcement` (21)
- **Memory subsystem:** `memory-confidence-labeling-and-system-message-separation` (17), `memory-ranking-recency-signal` (18)
- **Model provider content fidelity:** `outbound-multimodal-capability-gate` (19, Phase 1 only — Phase 2 needs a second, later ADR once RFC 019's blob-store seam exists), `cross-provider-reasoning-exclusion` (20)

### What's deferred (named residuals, not silently dropped)

Pulled from the individual approaches — these are explicitly out-of-scope items the architects and red-team agreed should be tracked separately rather than bundled in:

- **Gap 1:** the transport plumbing itself (HttpListener, TLS session, HTTP parser) can proceed in parallel with the new ADR — only the final wiring to McpServer/A2aServer is blocked.
- **Gap 3 / 12:** ~~a symmetric `FsRead`/`size_cap_bytes` capped-grant gate bug (same bug class as the confirmed `FsWrite` one) exists but was not in scope for this pass — should be filed as a tracked follow-up.~~ **[2026-08-14: CLOSED alongside gap 12, ADR-040]** — folded into the same fix rather than filed separately.
- **Gap 4:** plugin/MCP/A2A tool-name resolution (015 §1's fuller promise) is out of scope for the first registry cut, which only covers compiled-in native `Tool<...>` types.
- **Gap 5:** capability-bearing declarative tools stay unusable until `spec.capabilities` parsing lands — a second, explicitly separate change.
- **Gap 7:** `watch_resource` stays deferred pending 012/A2A; the mid-turn `ae::task<T>` suspension machinery is a separate, larger, already-documented project-wide gap.
- **Gap 15:** `AgentSessionRecord` still doesn't serialize `history_` (message content) — `RequireDurableAck`, once fixed, only closes the acknowledgment-protocol half of 005 §2 until that already-tracked gap also lands.
- **Gap 16:** wiring `AgentMetadata::agent_instructions` into a `ContextContribution` is out of scope — it needs an `AgentSession`-from-`AgentMetadata` construction point that doesn't exist yet.
- **Gap 19:** Phase 2 (real Media wire encoding) is blocked on RFC 019's blob-store seam, which doesn't exist anywhere in the tree; `Citation`/`Custom` content kinds and `Media` nested inside `ToolResult::content` remain unaddressed by either phase.
- **Gap 20:** whether wrapper compositions (`FailoverChatClient`/`ResilientChatClient`) are in scope for v1's cross-provider exclusion, or explicitly deferred with a documented limitation, is left as an open decision for the ADR.

---

## What to Do Next, Ordered by Priority

1. ~~**Fix the naming-lint namespace bug (gap 8) before anything else.**~~ **DONE (2026-08-13, ADR-025).**

2. ~~**Correct the milestone-status claim (gap 23) immediately**~~ **DONE (2026-08-13, ADR-026 + the
   2026-08-13 post-ADR-037 Quark-mention sweep, which also caught the marketing-site drift this item
   named).**

3. **Treat the three sandbox/native-jail findings (10, 11, 12) as the security-critical core of this audit.** These are the most direct threats to I2 ("no ambient authority"): Linux gives guests the whole host filesystem and process table, ShellRunner's quota gate is inverted, and the Windows fix is blocked on an unverified privilege assumption. Spike the Windows privilege question empirically first (cheap, fast, gates the whole approach), then take Linux and ShellRunner through design → red-team → prove → judge together since they share the "gate-only fix creates a silent bypass" failure pattern.

4. **Close the I3 (model output is never authority) gaps together: 16 and 21.** Both concern the taint mechanism failing to actually gate what it's supposed to gate — 16 at the instructions-injection channel, 21 at the content model's core accessors. These are foundational enough that fixing one without the other leaves an inconsistent taint story.

5. ~~**Fix the durability/ack gap (15)**...~~ **DONE (2026-08-14, ADR-043).** The premise this item was scheduled behind was itself wrong — no Store type-erasure question needed resolving; the seam already existed as a deliberate `concept`.

6. **Batch the remaining medium/low findings by subsystem** rather than one-by-one: the declarative-agent-surface group (3, 4, 5, 6 — 2 is now closed, see its own row) shares a single registry/validator design space and should go through one coordinated ADR sequence rather than four independent ones; the memory-subsystem group (17, 18) and the model-provider-fidelity group (19, 20) are each internally coupled the same way.

7. **Assign real ADR numbers before any of the above lands** — resolve the `ADR-025` collision across all 23 suggested titles by sequencing them (likely ADR-025 through ADR-047, in the priority order above) as part of scheduling this work, not as an afterthought at merge time.

No files were changed as part of producing this report; it synthesizes the audit's structured output as supplied, without adding findings beyond what was in that data.
