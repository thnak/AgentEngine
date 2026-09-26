# tests/workflow/: workflows (include/agentengine/workflow/, rt/workflow_*.hpp).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "workflow" ctest label for every test here (cmake/AgentEngineTests.cmake).

if(AGENTENGINE_WITH_HTTPS)
  # ---- Milestone 6 Phase A: 014 §1/§2, the graph as data and its SHARED validator ----------------
  # Every validator rule is exercised in both directions. A suite that only built correct graphs and
  # watched them pass would be satisfied by a validator that returned success unconditionally, so
  # each rule has a graph that trips it AND the nearest graph that does not. Includes the two cases
  # most easily got wrong: a cyclic graph must VALIDATE (014 §9 Q2 allows cycles; the reflection and
  # group-chat patterns need them), and the reachability walk must terminate on a self-loop rather
  # than hang. Pure data, no actors, no engine -- default suite.
  agentengine_add_test(test_workflow_graph_validation LIBS agentengine::core agentengine_warnings)

  # ---- OQ-19: the graph-layer half of the agent-executor CapabilitySet gate -- Executor::
  # capability_ceiling, check_workflow_executable(wf, contexts)'s contexts-aware overload, and the
  # YAML compiler's explicit refusal of an authored capability_ceiling key (I6: refused loudly, never
  # silently dropped). Pure data + the shared validator, no rt:: -- default suite.
  agentengine_add_test(test_workflow_agent_executor_gate LIBS agentengine::core agentengine_warnings)

  # ---- Milestone 6 Phase G: 014 §7's rendering and diffing (introspection.hpp) -------------------
  # Render is a TOTAL function -- the falsifiable bar is "a valid workflow that cannot be drawn" --
  # proven against a graph exercising all four executor kinds, all six edge kinds, a fallback
  # branch, and author ids containing characters (quotes) that would break naive Mermaid/DOT syntax
  # if node identifiers were sanitized author text instead of index-derived tokens. Diff is a plain
  # structural comparison: add/remove/change an executor, add/remove an edge, start/output/bound
  # changes, and an unchanged graph diffing to empty.
  agentengine_add_test(test_workflow_introspection LIBS agentengine::core agentengine_warnings TIMEOUT 120)

  # ---- A production-shaped multi-agent workflow (planner + concurrent specialists + fan-in + writer
  # + human publish gate + checkpoint/resume across a simulated restart), driven against REAL
  # OpenRouter calls -- see the .cpp's own top comment for the three specific bug classes this hunts
  # for that an offline/synthetic workflow test cannot reach: real failure-classification of a live
  # response, fan-in correctness under real jittered concurrency, and checkpoint/resume not re-doing
  # already-paid-for live work.
  agentengine_add_test(test_workflow_research_pipeline_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_workflow_research_pipeline_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- The SAME production pipeline shape, but each concurrent specialist is fed a large (~60K+
  # token) REAL reference corpus sliced from this repo's own design docs before the actual brief --
  # 2026-09-03, added at the project owner's request after the small-context test above showed only
  # ~1-2K tokens of context per call on the OpenRouter dashboard, nothing like a real document-
  # grounded production call. See the .cpp's own top comment for what this specifically hunts for.
  # Deliberately NOT labeled "live-network" -- tools/run-live-provider-tests.ps1's own `-L live-
  # network` selection (and any other sweep keyed on that label) must never pick this up as a side
  # effect; it needs its own explicit AGENTENGINE_RUN_LARGE_CONTEXT_LIVE_TESTS=1 opt-in (checked
  # inside the test itself) on top of the ordinary API-key gate, because a single run is well over
  # 100K billed tokens.
  add_executable(test_workflow_research_pipeline_large_context_live_e2e
    test_workflow_research_pipeline_large_context_live_e2e.cpp)
  target_compile_definitions(test_workflow_research_pipeline_large_context_live_e2e PRIVATE
    AGENTENGINE_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  target_link_libraries(test_workflow_research_pipeline_large_context_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_workflow_research_pipeline_large_context_live_e2e
    COMMAND test_workflow_research_pipeline_large_context_live_e2e)
  set_tests_properties(test_workflow_research_pipeline_large_context_live_e2e PROPERTIES
    LABELS "live-network-heavy" TIMEOUT 900)
endif()

# ---- ADR-037 Phase 3, Slice 1 -- agentengine::rt::WorkflowSupervisor, the Quark-actor-free
# replacement for workflow::WorkflowSupervisor's core superstep loop, fanning out through
# rt::ThreadPool instead of quark::ActorRef<FunctionExecutor>::ask<>.
agentengine_add_test(test_rt_workflow_supervisor LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 3, Slice 1 -- request-port behavior against rt::WorkflowSupervisor, superseding
# the old Quark-actor-based test_workflow_request_port(.cpp)/test_workflow_request_port_independent_qa
# (014 §4). E1/E2/E3/E5 and IQ1-IQ6, minus the two claims that are genuinely out of scope for `rt::`:
# E0 (shared, unchanged workflow/graph.hpp validator code) and the activation-passivation half of
# E1/E4 (there are no Quark activations in `rt::` for passivation to apply to).
agentengine_add_test(test_rt_workflow_supervisor_request_port LIBS agentengine::core agentengine_warnings)

# ---- ADR-169 (GitHub issue #65) -- caller admission on rt::WorkflowSupervisor's three entry points.
# The ownership half of what the file above only proved for id VALIDITY: knowing a live interaction_id
# was previously sufficient authority to resolve a suspended HITL interaction and name its routes.
# A1-A12: legacy compatibility, owner/stranger, cross-tenant, single-hop delegation, require_caller()
# strict mode, unset-owner fail-closed, run_workflow()/continue_workflow() parity, survival across
# initialize()/restore_from_record(), nested (ADR-157) forwarding + inheritance, and the audit event.
agentengine_add_test(test_rt_workflow_supervisor_admission LIBS agentengine::core agentengine_warnings)

# ---- OQ-19: the runtime half of the agent-executor CapabilitySet bridge (rt/agent_workflow_
# executor.hpp) -- AgentExecutorBodyTag's positive control, WorkflowSupervisor::initialize()'s
# structural-marker refusal (closing test_workflow_graph_validation.cpp's own stale-comment gap),
# a real end-to-end dispatch through a scripted AgentSession, cyclic history accumulation within one
# run, the concurrent-same-node duplicate-delivery quarantine (both the default `fail` and a
# `propagate` failure policy), and the checkpoint/resume documented fresh-session limitation.
agentengine_add_test(test_rt_agent_workflow_executor LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 3 -- 014 §7's live-view bullet (WorkflowSupervisor::enable_live_view) against
# rt::WorkflowSupervisor -- supersedes the old Quark-actor-based test_workflow_live_view.cpp (LV1/LV2,
# reused verbatim). One WorkflowLiveEvent per superstep boundary over a real agentengine::stream<T>,
# checked from the executor-state angle, plus the negative control (a round that fails the whole run
# produces no live event, proven by draining to empty, not just an unchecked assumption).
agentengine_add_test(test_rt_workflow_live_view LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 3, Slice 1 -- bound/failure-policy behavior (B3-B5, D1-D4) ported from the two
# retired Quark-actor-based test_workflow_superstep(.cpp)/test_workflow_failure(.cpp) onto
# rt::WorkflowSupervisor directly -- supersedes both.
agentengine_add_test(test_rt_workflow_supervisor_failure_policies LIBS agentengine::core agentengine_warnings)

# ---- GitHub issue #52 fix proof: `propagate`/`fallback` on a `fan_in` edge whose target a SIBLING
# also delivers to normally in the SAME round -- a real gap D4 above never exercised, found live via
# tests/test_workflow_research_pipeline_live_e2e.cpp, now fixed (route_from()'s deliver_or_merge()/
# seed_fan_in_holds()/deliver_to_fan_in()/resolve_fan_in_await(), workflow_supervisor.hpp) -- see this
# file's own top comment for the full trace.
agentengine_add_test(test_workflow_fanin_concurrent_failure_policy_fix
  LIBS agentengine::core agentengine_warnings)

# ---- GitHub issue #62 fix proof: a real, DIFFERENT (broader) gap than the one above -- an ordinary
# `fan_in` target did not wait for every declared source before dispatching -- when two sources
# resolved at genuinely different round counts (no failure/fallback involved at all), the target
# dispatched TWICE, and the second invocation silently overwrote the first. Found live via
# tests/test_workflow_research_pipeline_large_context_live_e2e.cpp, now fixed by generalizing #52's
# fix (seed_fan_in_holds() pre-registers every multi-source fan_in target's barrier up front, not just
# a fallback's direct-rejoin shape) -- see this file's own top comment for the full trace.
agentengine_add_test(test_workflow_fanin_uneven_round_sources_fix LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 3 -- routing behavior (fan_in/switch_case/multi_selection/cyclic, plus retry
# composing with each) ported from the two retired Quark-actor-based
# test_workflow_patterns(.cpp)/test_workflow_patterns_resilience(.cpp) onto rt::WorkflowSupervisor
# directly -- supersedes both, including the I3 routing boundary check (RF-1/RF-2).
agentengine_add_test(test_rt_workflow_supervisor_patterns LIBS agentengine::core agentengine_warnings)

# ---- ADR-051 (gap-audit finding 22 / 014 §8 G3) -- the 10^3-seed scheduling shuffle test, never
# built before this file: shuffled intra-round completion order produces identical output, and a
# deliberately order-dependent executor is caught by the same sweep.
agentengine_add_test(test_rt_workflow_supervisor_scheduling_shuffle
  LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- agentengine::rt::WorkflowSupervisor's time-travel surface (014 §5's "rewind to
# any retained checkpoint"), wiring rt::AppendLogStore into the gap workflow_supervisor.hpp's own
# Slice 2 banner named as deferred. Additive over Slice 2's existing single-slot
# save_workflow_checkpoint/load_workflow_checkpoint (rt::SessionStore, still used for ordinary
# crash-recovery resume) -- this file answers the genuinely different "rewind to ANY retained
# checkpoint" claim a single-slot store cannot serve.
agentengine_add_test(test_rt_workflow_time_travel LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- 014 §8's own promotion gate G2, reproven against agentengine::rt::WorkflowSupervisor
# -- supersedes the old Quark-actor-based test_workflow_checkpoint_g2.cpp. A 20-node chain, killed
# (simulated: a genuinely new instance restore_from_record()'d from nothing but the checkpoint) at
# EVERY one of its 20 superstep boundaries -- every resume must complete with output identical to an
# uninterrupted control. 21 plain host-held WorkflowSupervisor instances in sequence, never
# concurrent -- no Engine/Activation stack needed, so the old 300s timeout is dropped.
agentengine_add_test(test_rt_workflow_checkpoint_g2 LIBS agentengine::core agentengine_warnings)

# ---- ADR-149 (GitHub issue #28): a Magentic-shaped workflow convenience API. Design draft:
# docs/planning/magentic-workflow-convenience-api-design-draft.md.
agentengine_add_test(test_rt_workflow_stall_reset_bounds LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_workflow_magentic_builder LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_workflow_magentic_plan_signoff LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_rt_workflow_checkpoint_manager LIBS agentengine::core agentengine_warnings)

# ---- Adjacent to GitHub issue #35 (not #33): workflow_as_executor_body(), a Workflow-as-participant
# adapter. Design draft: docs/planning/workflow-as-executor-body-adapter-design-draft.md.
agentengine_add_test(test_rt_workflow_as_executor LIBS agentengine::core agentengine_warnings)

# ---- GitHub issue #35: WorkflowChatClient (rt/workflow_as_chat_client.hpp), a Workflow-as-ChatClient
# adapter -- the OTHER direction from workflow_as_executor_body() above. Design draft, ten independent
# red-team rounds: docs/planning/workflow-as-chatclient-adapter-design-draft.md.
agentengine_add_test(test_rt_workflow_as_chat_client LIBS agentengine::core agentengine_warnings)

# ---- ADR-153 (issue #29): WorkflowSupervisor's fine-grained, genuinely-live per-executor event
# stream. Design draft: docs/planning/workflow-event-stream-design-draft.md.
agentengine_add_test(test_rt_workflow_event_stream LIBS agentengine::core agentengine_warnings)

# ---- ADR-157 (issues #33/#38): executor_kind::sub_workflow's real runtime bridge, with nested
# request_port proxying. Design draft: docs/planning/sub-workflow-nested-request-port-design-draft.md.
agentengine_add_test(test_rt_workflow_sub_workflow LIBS agentengine::core agentengine_warnings)

# ---- GitHub issue #37: mid-run cancellation for WorkflowSupervisor. Design draft:
# docs/planning/workflow-mid-run-cancellation-design-draft.md.
agentengine_add_test(test_rt_workflow_cancellation LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase F2 -- 015 §3's Workflow-document compiler (workflow\yaml_compiler.hpp):
# compiles a parsed YAML Workflow document to graph.hpp's own Workflow, checked by the SAME
# validate_workflow() the C++ WorkflowBuilder form uses (I6) -----------------------------------
agentengine_add_test(test_workflow_yaml_compiler LIBS agentengine::core agentengine_warnings)

# ---- Workflow executor worktree scoping (014 §1 <-> 025 §3, ADR-032) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto (compute_digest)
# dependency as A1/B1/B2/B3's own tests above.
if(WIN32)
  agentengine_add_test(test_workflow_worktree_scoping
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Merge-on-join wiring (025 §4, ADR-055 §6 follow-up) -- bridges worktree_scoping.hpp's
# make_merge_on_join_hook() with rt::WorkflowSupervisor's new MergeOnJoinHook for real, driving a
# branch-mode executor through the supervisor to completion for the first time in this codebase.
if(WIN32)
  agentengine_add_test(test_rt_workflow_supervisor_merge_on_join
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()
