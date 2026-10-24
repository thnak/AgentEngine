# Decisions — the ADR record

An **ADR** records a contested, hot-path or security-critical decision that went through
`design → red-team → prove → judge`: competing designs, attacked adversarially, proven with executed
evidence (compilers, sanitizers, measurements, positive controls), then decided on that evidence.

**The record was reset on 2026-10-20** (project-owner direction). [ADR-242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md)
is the first live ADR. ADR-001 to ADR-241 are kept, unedited, in [`archive/`](archive/README.md).

## When an ADR is required

- A **security-critical** choice: capability representation, isolation boundary, taint, approval
  binding, secret handling.
- A **hot-path** choice measured by a 023 budget.
- A choice where **credible designs disagree** and reading the code cannot settle it.
- A **promotion** of an RFC from Draft to Proven.

Ordinary implementation choices do not need one. An ADR for everything is an ADR for nothing.

## What an ADR contains

1. **The question**, stated so that it has a wrong answer.
2. **The competing designs**, each steelmanned.
3. **Falsifiable claims**, each paired with the experiment that would disprove it.
4. **The red-team attack**, hardest where the design claims to be fast or safe.
5. **The executed evidence**, with positive controls (a test that cannot fail proves nothing).
6. **Per-claim verdicts**: CORRECT / WRONG / INCONCLUSIVE, decided by observed output.
7. **The decision**, the specs it binds, and the disclosed residuals.

**Keep it short.** An ADR states the decision and its evidence; it is not the work log. Aim for a few
hundred lines. Long design records, round-by-round red-team transcripts and raw measurements go in
`docs/planning/` and are linked. (The archive's longest ADR ran to 6,100 lines; that is what this rule
prevents.)

## Naming and status

`ADR-NNN-<kebab-case-question>.md`, numbered sequentially from **243**. Numbers 001–241 belong to the
archive and are **never reused**, so a code comment citing `ADR-084` always means the archived one.
Status is one of Proposed, Judged, Superseded (pointing forward). Only the project owner marks Judged.

## The archive, and what still binds

- **Archived Judged or Accepted ADRs still bind**, except where a live ADR supersedes them (ADR-242's
  header lists what it supersedes). They are the reasoning behind shipped code; a change to what they
  decided needs a live ADR.
- **Archived Proposed, Draft or Split ADRs are history, not decisions.** Code built under them stays,
  and its tests remain the gate. Making such a decision binding, or changing it, takes a live ADR.
- Code comments keep citing archived numbers; paths now read `decisions/archive/ADR-NNN-…`.

Still binding (98, from the archived index's status column): [001](archive/ADR-001-shellrunner-grammar-and-dispatch.md), [002](archive/ADR-002-pythonrunner-embedding-and-mediation.md), [003](archive/ADR-003-caller-aware-import-gating.md), [004](archive/ADR-004-appcontainer-native-jail-windows-backend.md), [005](archive/ADR-005-capability-bearer-tokens-cross-process.md), [006](archive/ADR-006-agent-spawn-depth-budget-bound.md), [007](archive/ADR-007-span-level-taint-vs-per-item.md), [008](archive/ADR-008-wasm3-cold-start-vs-wasmtime.md), [009](archive/ADR-009-capability-set-enforcement-mechanism.md), [010](archive/ADR-010-wasm-component-host-manifest-capability-binding.md), [011](archive/ADR-011-first-party-egress-proxy.md), [012](archive/ADR-012-sandbox-profile-template-parameter-kind.md), [013](archive/ADR-013-https-egress-tls-client.md), [014](archive/ADR-014-worktree-mount-path-canonicalization.md), [015](archive/ADR-015-shellrunner-grammar-parser-fuzzing.md), [016](archive/ADR-016-provider-egress-address-policy.md), [017](archive/ADR-017-stream-consumer-cancellation-signal.md), [018](archive/ADR-018-agent-session-real-chat-client-wiring.md), [019](archive/ADR-019-incremental-streaming-response.md), [020](archive/ADR-020-portable-reasoning-effort.md), [023](archive/ADR-023-response-format-codec-seam.md), [026](archive/ADR-026-milestone-status-doc-accuracy-and-drift-lint.md), [025](archive/ADR-025-naming-lint-namespace-scope-and-027-vocabulary-diagram.md), [024](archive/ADR-024-skill-scoped-tool-and-mount-wiring.md), [028](archive/ADR-028-session-scoped-stateful-tools.md), [027](archive/ADR-027-agent-session-tool-call-loop.md), [031](archive/ADR-031-spawn-cost-budget-actor-primitive.md), [032](archive/ADR-032-workflow-executor-worktree-scoping.md), [033](archive/ADR-033-middleware-model-call-chain.md), [030](archive/ADR-030-session-scoped-codeact-wiring.md), [029](archive/ADR-029-suspend-for-human-approval.md), [034](archive/ADR-034-agentsession-streaming-turn-loop.md), [035](archive/ADR-035-chatclient-streaming-completeness.md), [036](archive/ADR-036-model-call-gateway.md), [038](archive/ADR-038-rt-project-passivation-semantics.md), [039](archive/ADR-039-inbound-transport-host-pluggable.md), [037](archive/ADR-037-remove-quark-as-core-runtime.md), [052](archive/ADR-052-shell-registered-tool-dispatch-scope-correction.md), [051](archive/ADR-051-workflow-scheduling-shuffle-test.md), [050](archive/ADR-050-usage-usagedetails-canonical-name-reconciliation.md), [049](archive/ADR-049-cross-provider-reasoning-exclusion.md), [048](archive/ADR-048-outbound-media-capability-gate.md), [047](archive/ADR-047-memory-ranking-product-formula.md), [046](archive/ADR-046-memory-confidence-labels-and-system-message-separator.md), [045](archive/ADR-045-json-schema-validator-subset.md), [044](archive/ADR-044-agent-workflow-description-version.md), [043](archive/ADR-043-ack-durability-policy.md), [042](archive/ADR-042-context-instructions-taint-channel.md), [041](archive/ADR-041-appcontainer-ace-leak-accepted-residual.md), [056](archive/ADR-056-fs-quota-capability-gate-fix.md), [055](archive/ADR-055-conflict-evidence-materialization.md), [054](archive/ADR-054-tool-registry-name-keyed-resolution.md), [053](archive/ADR-053-schedule-wakeup-standing-effect.md), [063](archive/ADR-063-retrieval-augmented-context-provider-shape.md), [064](archive/ADR-064-recall-tool-sync-invoke-vs-async-embedder.md), [065](archive/ADR-065-tool-optimizer-provider.md), [066](archive/ADR-066-context-provider-attribution-provenance.md), [069](archive/ADR-069-content-triggered-model-response-replay.md), [070](archive/ADR-070-host-configurable-responsibility-boundary.md), [071](archive/ADR-071-native-unsandboxed-process-execution-providers.md), [073](archive/ADR-073-unified-streaming-gateway-call-stream-tool-argument-result-streaming-ask-stream.md), [074](archive/ADR-074-composed-context-provider-consolidation.md), [075](archive/ADR-075-context-budget-fail-closed.md), [076](archive/ADR-076-undeclared-tool-call-leak-refusal.md), [077](archive/ADR-077-agent-executor-capabilityset-bridge.md), [078](archive/ADR-078-tool-call-hook-stage.md), [079](archive/ADR-079-agent-spawn-runtime-and-capability-minting.md), [081](archive/ADR-081-jailed-python-worker-process-slice-1.md), [082](archive/ADR-082-native-jail-promotion-gate-008-9.md), [083](archive/ADR-083-linux-native-jail-pivot-root-containment.md), [084](archive/ADR-084-kata-backend-slice-1.md), [085](archive/ADR-085-jailed-python-worker-slice-2-handle-relay.md), [086](archive/ADR-086-kata-backend-slice-2-enforcement.md), [152](archive/ADR-152-workflow-event-streaming.md), [153](archive/ADR-153-agent-memory-codeact-bridging.md), [154](archive/ADR-154-agent-output-codeact-module.md), [155](archive/ADR-155-agent-progress-codeact-module.md), [156](archive/ADR-156-agent-notes-write-back.md), [157](archive/ADR-157-sub-workflow-nested-request-port.md), [159](archive/ADR-159-workflow-mid-run-cancellation.md), [175](archive/ADR-175-threadpool-parked-job-completion.md), [176](archive/ADR-176-execution-surface-image-identity.md), [177](archive/ADR-177-stream-retry-in-session.md), [180](archive/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md), [199](archive/ADR-199-agent-session-non-template-core.md), [200](archive/ADR-200-workflow-supervisor-out-of-line.md), [201](archive/ADR-201-tool-pipeline-out-of-line.md), [202](archive/ADR-202-one-message-json-codec.md), [203](archive/ADR-203-one-base64-one-process-identity.md), [204](archive/ADR-204-recordings-carry-attribution.md), [205](archive/ADR-205-strict-relay-base64.md), [206](archive/ADR-206-provider-chat-clients-out-of-line.md), [207](archive/ADR-207-sandbox-ledger-recording-headers-out-of-line.md), [208](archive/ADR-208-test-driver-real-tools-and-tool-doubles.md), [210](archive/ADR-210-test-driver-workflow-tool-group.md), [212](archive/ADR-212-deterministic-tool-output-declaration.md), [235](archive/ADR-235-batch-inference-coalescing.md), [236](archive/ADR-236-batch-recording-and-replay.md).

## Index

| ADR | Question | Status |
|---|---|---|
| [242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md) | Project-owner direction reset (2026-10-20): should AgentEngine stay a Windows-first SDK with one in-process Python, `native-jail` first and no networking, or become a Linux-server-first engine developers build multi-user agent applications on, where every option is chosen explicitly, invariants are never options, and every agent runs inside a `SandboxBackend` that hosts a worktree? | **Judged (2026-10-20, project-owner sign-off on PR #209).** Red-teamed in five rounds; owner decisions settled. |
| [243](ADR-243-layer-and-developer-api.md) | What is the public C++ API and layer structure, so that a developer builds a multi-user agent server in a few dozen lines, with every managed piece replaceable? | **Proposed (2026-10-20), revision 2 (2026-10-24).** Directories are layers (zero overrides, no cycles); four non-template handles (`Engine`, `AgentSpec`, `Session`, `Server`); per-session seams are factories; a concept plus an `Any…` holder per seam; ceiling refusal per ADR-244; line budgets 15 / 30 / 60 in CI. Red-team rounds 1 and 2 folded in; ADR-245's amendments folded in. |
