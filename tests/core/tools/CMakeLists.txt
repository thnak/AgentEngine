# tests/core/tools/: the tool pipeline, tool registry and approvals (006).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/tools" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- Milestone 4 Phase F1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Idempotency key derivation {run_id, turn_index, call_index, argument_digest}, wired into the
# real (M2) tool_pipeline.hpp invocation pipeline.
agentengine_add_test(test_idempotency_key LIBS agentengine::core agentengine_warnings)

# ---- Milestone 4 Phase F4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# 019 §6's rewind-then-reexecute rule: effect_class/EffectClass<C>, authorize_reexecution(), proven
# against a real manually-triggered invoke_tool() re-execution under the same idempotency key.
agentengine_add_test(test_effect_reexecution LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-197-reject-malformed-tool-arguments.md (issue #105): argument text that is not
# valid JSON is refused at 006 §3 step 2 as tool.malformed_arguments, never coerced to {} -- proven
# with an all-optional-arguments tool through invoke_tool, admit_call, background_task and a real
# AgentSession round.
agentengine_add_test(test_tool_call_malformed_arguments LIBS agentengine::core agentengine_warnings)

# ADR-192: unattended approvals (the approval half of unattended mode; the delivery half needs HTTPS and is
# test_unattended_mode). Off by default, audited, narrowed by auto_deny and a veto; a stateful decider keeps its state.
agentengine_add_test(test_unattended_approvals LIBS agentengine::core agentengine_warnings)

# ADR-196 (issues #104/#107/#108), ADR-198 (issue #106) and the round-3 red-team fixes to ADR-191/192: the session's
# resume paths (tool surface, per-call decisions, approver, stale per-interaction state) and the lesson registry.
agentengine_add_test(test_approval_resume LIBS agentengine::core agentengine_warnings)

# ---- ADR-030 -- session-scoped CodeAct wiring: the runner-binding claim/fail-closed primitive.
agentengine_add_test(test_codeact_runner_binding LIBS agentengine::core agentengine_warnings)

# ---- ADR-061 §46 prove phase -- ApprovalDecider widened to see the calling Principal, closing R27's
# `approve_` finding (decisions/ADR-061) ---------------------------------------------------------------
agentengine_add_test(test_approval_decider_principal LIBS agentengine::core agentengine_warnings)

# ---- ADR-054 (gap-4/gap-5 closure) -- ToolRegistry / ToolTable::from_names() (core/tool_registry.hpp).
agentengine_add_test(test_tool_registry LIBS agentengine::core agentengine_warnings)

# ---- Ten-step tool invocation pipeline (M2 Phase B tasks B2/B3, 006 §3, miniature §8 G2) --------
# Header-only (core/tool_pipeline.hpp), core-tier -- links only agentengine::core.
agentengine_add_test(test_tool_pipeline LIBS agentengine::core agentengine_warnings)

# ---- ADR-158: tool concurrency-exclusivity policy (declared tag only, no pipeline logic yet, ----
# matching Parallelizable's own existing state -- 006 §8 G4 is still deferred). Header-only.
agentengine_add_test(test_tool_exclusivity_group LIBS agentengine::core agentengine_warnings)

# ---- ADR-160: 006 §8 G4's parallel-batch scheduler -- partition_batch() in isolation, no I/O, no --
# AgentSession. Header-only (core/tool_pipeline.hpp).
agentengine_add_test(test_tool_batch_partition LIBS agentengine::core agentengine_warnings)

# ---- ADR-160: real, executed proof that AgentSession::dispatch_tool_calls() genuinely runs an -----
# eligible batch concurrently end to end (rendezvous, ExclusivityGroup mutual exclusion, and append-
# order determinism under deliberately out-of-order physical completion). Header-only.
agentengine_add_test(test_tool_batch_parallel_dispatch LIBS agentengine::core agentengine_warnings)

# ---- Tool-result-to-BlobRef promotion (006 §7 / 028 §2), first-party-tools Track A --------------
# Header-only (core/tool_pipeline.hpp's normalize_success, shared by invoke_tool/background_task),
# core-tier -- links only agentengine::core.
agentengine_add_test(test_tool_result_blob_promotion LIBS agentengine::core agentengine_warnings)

# ---- Skills Phase 2: core/skill_tool_scoping.hpp -- pure ToolTable filtering plus the negative
# control proving invoke_tool actually rejects a real tool absent from the scoped table (not just
# cosmetic list-shortening). No skills/sandbox dependency -- core-tier only.
agentengine_add_test(test_tool_table_scoping LIBS agentengine::core agentengine_warnings)

# ---- Capability-handle-reuse-denial, at the pipeline level (M2 Phase B task B4, 006 §8 G3) ------
agentengine_add_test(test_tool_pipeline_capability_reuse LIBS agentengine::core agentengine_warnings)

# ---- issue #80: an execution surface reports WHICH IMAGE a command ran in. Real Docker daemon, same
# RESOURCE_LOCK as every other daemon-backed test here (see test_composed_sandbox_providers_live's own
# comment for the CI failure that lock exists because of). /bigobj for the same mechanical reason
# test_composed_sandbox_providers_live needs it: this TU instantiates MandatorySandboxProvider too.
# ADR-176 §14/§15: the image-provenance shape gate's controls. Pure JSON-schema shape checking -- NO
# Docker daemon, deliberately, so that unlike the two binaries that apply the gate for real this one runs
# on every CI leg including the Windows ones, which exclude every daemon-backed test.
agentengine_add_test(test_image_provenance_shape LIBS agentengine::worktree_store agentengine_warnings)
