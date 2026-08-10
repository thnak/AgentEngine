# Full-codebase ADR-conformance and gap-closure audit

**Run:** 2026-08-10, `wf_6d9637fb-786`, 130 agents (~9.5M tokens, ~41 min) · **Status:** audit output,
not itself a spec or ADR — every finding below still needs its own ADR before anything is built
against it, per the process this document's own findings confirm the project actually follows.

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
| 1 | No real network listener despite ADR-021/022 deciding its shape | **High** | needs_adr | Build the already-Judged listener/TLS/parser now; gate wiring it to McpServer/A2aServer on a new ADR that fixes principal propagation and closes the A2A cross-tenant task-history leak. |
| 8 | 027's naming-lint gate fails on main (76 unlisted + 112 suppressed names) | **High** | needs_adr | Fix the scanner's namespace-matching bug first — it's blind to ~40 files under `agentengine::{trust,sandbox,workflow}`, the highest-risk modules — before any bulk table reconciliation, or "G3 satisfied" becomes a false claim. |
| 10 | Linux native-jail gives the guest full host FS read/write + process visibility | **High** | needs_adr | Build pivot_root+bind-mount containment, but the cited path-validation primitive is ADR-014's already-rejected, TOCTOU-vulnerable design — must use the accepted open-then-verify primitive, fix a repeated-exec `rmdir` bug that breaks a currently-passing test, and narrow the blanket `/usr` bind-mount. |
| 12 | ShellRunner write builtins unconditionally deny writes under any quota-capped FsWrite grant | **High** | needs_adr | The gate-only fix would flip a safe denial bug into a silent, unbounded quota bypass (MediatedFileSystemAdapter never enforces quota on writes) — must ship the gate fix and live usage enforcement together. |
| 15 | AgentSession acks a turn before it's durable; no `at_most_once_ack` escape hatch | **High** | needs_adr | Add an `AckPolicy` (default `AtMostOnceAck`, opt-in `RequireDurableAck`), but the proposed insertion point sits *after* `run_finished` is already emitted (breaks a tested invariant), and the required type-erased Store seam doesn't exist yet. |
| 16 | `ContextContribution.instructions` computed but silently dropped before reaching the model | **High** | needs_adr | Route instructions through the existing `role::system` wire channel — but that channel is taint-blind end-to-end today (tainted memory content already rides it unchecked by both backends); must close that first. |
| 19 | Image/Audio/Video/File (and Anthropic Reasoning) content silently dropped outbound despite declared capability bits | **High** | needs_adr | Split into Phase 1 (fail-closed capability gate + symmetric drop-signal, implementable now) and Phase 2 (real wire encoding), which is blocked on RFC 019's blob-store seam — which does not exist anywhere in the tree. |
| 23 | CLAUDE.md/README claim Milestones 7-9 "have not started" though M7 is substantially built | **High** | needs_adr | Correct the docs and add a CI lint modeled on `naming_lint.py` — but a third, worse-offending location exists (the marketing site) and the proposed lint's regex self-collides with its own corrected text; both must be fixed before landing. |
| 2 | AgentMetadata/Workflow have no description/version fields | Medium | needs_adr | Add optional fields wired through 5 call sites — sound, but must close a naming collision with `Tool<>`'s own `description` static and state the I6 equivalence guarantee honestly (proven only for the maintained corpus, not structurally). |
| 3 | No generic JSON Schema 2020-12 validator | Medium | needs_adr | Build `core/json_schema_validator.hpp` with the proposed keyword subset/budgets — but wire it at `invoke_tool()`'s InvokeFn construction site (not the MCP dispatcher), add the missing outbound client-side strict check, and close a regex-DoS/`$ref`-cycle budget gap. |
| 4 | No tool/capability name-keyed registry | Medium | needs_adr | The submitted proposal was placeholder text with no real content — a real design must resolve I2 capability-ceiling binding, I3 resolution timing, namespace squatting, and WASM-ABI conformance from scratch. |
| 5 | ToolTable has zero runtime construction API (name-keyed half) | Medium | needs_adr | Add a `ToolRegistry` wired through `compile_agent_document` — but the proposed nullptr-fail-closed policy inverts an existing convention and breaks a currently-passing test (015 §2's own worked example). |
| 7 | M7 Phase G gate 006 §6b G6 (`schedule_wakeup`) is unprovable as written | Medium | needs_adr | Give `schedule_wakeup` a real `StandingEffect` producer — but must enforce `Schedule<max_horizon>` at arm time (currently unbounded, a live I2 gap) and name the missing `ReminderService`-access seam. |
| 9 | 027's canonical name `UsageDetails` has drifted to `Usage` in code | Medium | needs_adr | A mechanical rename only works once 003 §6 and 004 (which normatively say `Usage`, not `UsageDetails`) are reconciled too — otherwise it trades one spec/code drift for another. |
| 11 | Windows native-jail leaks curated host files via inherited AppContainer ACEs | Medium | needs_adr | The deny-only-SID fix is blocked pending an empirical spike: the proposed launch APIs (`CreateProcessAsUser`/`WithTokenW`) need privileges a standard non-admin deployment account likely doesn't hold. |
| 13 | Shell-dispatched registered Tools bypass the 006 §3 tool pipeline | Medium | needs_adr | Route shell tool calls through `bridge_tool_call` like Python's bridge already does — but fix a null-`tool_bridge` crash, stop collapsing capability/approval denials into continuable errors, and give `call_index` a real source. |
| 17 | MemoryProvider renders ModelInferred and UserStated items identically | Medium | needs_adr | Add a confidence-label prefix — but must pair it with fixing Anthropic's zero-separator system-message concatenation (labels visually bleed together) and address a new label-forgery surface the fix itself introduces. |
| 18 | Default memory ranking is additive, not salience×recency×keyword | Medium | needs_adr | Fix the formula to a literal product (sound) — but the proposed recency signal is non-monotonic on overwrite and adds a real O(item-count) traversal; use the Store's actual sequence number instead. |
| 20 | Cross-provider Reasoning-exclusion design (003 §8 Q2) never implemented | Medium | needs_adr | Wire `ChatClientId` provenance through — but a real production call site (`HistoryAndSkillsProvider`) was missed, and `FailoverChatClient`'s Primary-only identity would falsely flag ordinary failover as cross-provider. |
| 21 | Content model never uses type-level `Tainted<T>` for text/structured fields | Medium | needs_adr | Proposed accessors are additive and bypassable — the raw fields stay directly readable at every real consumption boundary (cli_chat.cpp, mcp/server.hpp, tool_bridge.hpp), so I3 remains structurally unenforced. |
| 22 | 014 §8 G3 (10³-seed scheduling shuffle test) never built, undisclosed | Medium | needs_adr | Write the missing test per the M6 breakdown's own already-designed architecture — but "the test exists" is falsely claimed in more than the two spots the architect named; all must be retracted together. |
| 6 | `output_schema $ref` never resolved by the declarative compiler | Low | needs_adr | Add an opt-in `SchemaRefResolver` — but must extract the *full* enforceability check (not half), reuse `worktree.hpp`'s tested path validator for Windows-safe traversal checks, and not overstate unbuilt digest/signing infrastructure as a safety net. |
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
- **Gap 3 / 12:** a symmetric `FsRead`/`size_cap_bytes` capped-grant gate bug (same bug class as the confirmed `FsWrite` one) exists but was not in scope for this pass — should be filed as a tracked follow-up.
- **Gap 4:** plugin/MCP/A2A tool-name resolution (015 §1's fuller promise) is out of scope for the first registry cut, which only covers compiled-in native `Tool<...>` types.
- **Gap 5:** capability-bearing declarative tools stay unusable until `spec.capabilities` parsing lands — a second, explicitly separate change.
- **Gap 7:** `watch_resource` stays deferred pending 012/A2A; the mid-turn `ae::task<T>` suspension machinery is a separate, larger, already-documented project-wide gap.
- **Gap 15:** `AgentSessionRecord` still doesn't serialize `history_` (message content) — `RequireDurableAck`, once fixed, only closes the acknowledgment-protocol half of 005 §2 until that already-tracked gap also lands.
- **Gap 16:** wiring `AgentMetadata::agent_instructions` into a `ContextContribution` is out of scope — it needs an `AgentSession`-from-`AgentMetadata` construction point that doesn't exist yet.
- **Gap 19:** Phase 2 (real Media wire encoding) is blocked on RFC 019's blob-store seam, which doesn't exist anywhere in the tree; `Citation`/`Custom` content kinds and `Media` nested inside `ToolResult::content` remain unaddressed by either phase.
- **Gap 20:** whether wrapper compositions (`FailoverChatClient`/`ResilientChatClient`) are in scope for v1's cross-provider exclusion, or explicitly deferred with a documented limitation, is left as an open decision for the ADR.

---

## What to Do Next, Ordered by Priority

1. **Fix the naming-lint namespace bug (gap 8) before anything else.** It is currently blind to `agentengine::trust`, `::sandbox`, and `::workflow` — the exact modules the other high-severity findings live in. Any other reconciliation work risks being validated by a broken gate, and 027 §6's namespace diagram needs updating to match reality first.

2. **Correct the milestone-status claim (gap 23) immediately** — it's the cheapest fix on the list (docs + a CI lint) and it's actively misleading anyone using CLAUDE.md/README as a status source. Fix the marketing site's `ApiProtocolStatus.tsx` in the same pass (worse-offending, currently claims *zero* implementation exists for MCP/A2A/AG-UI) and test the proposed lint regex against its own corrected paragraph before wiring it into CI.

3. **Treat the three sandbox/native-jail findings (10, 11, 12) as the security-critical core of this audit.** These are the most direct threats to I2 ("no ambient authority"): Linux gives guests the whole host filesystem and process table, ShellRunner's quota gate is inverted, and the Windows fix is blocked on an unverified privilege assumption. Spike the Windows privilege question empirically first (cheap, fast, gates the whole approach), then take Linux and ShellRunner through design → red-team → prove → judge together since they share the "gate-only fix creates a silent bypass" failure pattern.

4. **Close the I3 (model output is never authority) gaps together: 16 and 21.** Both concern the taint mechanism failing to actually gate what it's supposed to gate — 16 at the instructions-injection channel, 21 at the content model's core accessors. These are foundational enough that fixing one without the other leaves an inconsistent taint story.

5. **Fix the durability/ack gap (15)** — turns can currently be acknowledged to a caller before they're durable, with no opt-out honestly declared. This is a correctness-under-crash problem, independent of the sandbox work, and should be scheduled once the Store type-erasure question is resolved.

6. **Batch the remaining medium/low findings by subsystem** rather than one-by-one: the declarative-agent-surface cluster (2, 3, 4, 5, 6) shares a single registry/validator design space and should go through one coordinated ADR sequence rather than five independent ones; the memory-subsystem cluster (17, 18) and the model-provider-fidelity cluster (19, 20) are each internally coupled the same way.

7. **Assign real ADR numbers before any of the above lands** — resolve the `ADR-025` collision across all 23 suggested titles by sequencing them (likely ADR-025 through ADR-047, in the priority order above) as part of scheduling this work, not as an afterthought at merge time.

No files were changed as part of producing this report; it synthesizes the audit's structured output as supplied, without adding findings beyond what was in that data.
