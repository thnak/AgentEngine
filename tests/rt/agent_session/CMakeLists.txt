# tests/rt/agent_session/: rt::AgentSession, the session turn loop (include/agentengine/rt/agent_session*.hpp).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "rt/agent_session" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- ADR-037 Phase 2 -- porting test_agent_session_checkpoint.cpp's D1 (restart doesn't remint a
# run_id) and D2 (CheckpointCadence<N> fires at the exact expected turns over a real 7-turn
# sequence, and the durable/live-state lag is honestly observable) onto rt::AgentSession directly.
# Split into two files since the two claims are genuinely independent -- supersedes the old file.
agentengine_add_test(test_rt_agent_session_checkpoint_restart LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_rt_agent_session_checkpoint_cadence LIBS agentengine::core agentengine_warnings)

if(AGENTENGINE_WITH_HTTPS)
  # A real AgentSession (not the raw ChatClient layer above) driven through a genuinely hard,
  # multi-tool, multi-turn, forced-parallel-tool-call conversation against a real remote model. Also
  # the live proof for two real bugs this file's own prerequisite fixed (see its own top comment):
  # AgentSession silently dropping declared tools from its outbound request, and the OpenAI backend
  # mishandling a Message carrying multiple ToolResult items (the parallel-tool-call reply shape).
  # ADR-037: ported off quark::TestKit<Session> onto agentengine::rt::AgentSession directly --
  # supersedes the old test_agent_session_live_multitool_e2e.cpp (removed).
  agentengine_add_test(test_rt_agent_session_live_multitool_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_rt_agent_session_live_multitool_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ADR-029's suspend-for-approval mechanism (HITL), driven by a REAL model's own tool calls instead
  # of a scripted ChatClientT (every offline suspend/approval test uses a scripted one). Hunts for
  # bugs in the suspend/resume/approval-surface machinery itself: a misleading approval_requested
  # fan-out over a mixed gated/ungated round (BUG-1), collateral denial of an ungated call sharing a
  # round with a gated one (BUG-2), an approval surface that exposes no tool_name/arguments for a
  # human to actually decide on (BUG-3), plus softer "mid-conversation drop" (does history_ survive a
  # real suspend/resume across multiple start_run() calls) and "no-explanation" (does a real model's
  # own reply actually explain a denial to the user) probes. See the file's own top comment.
  agentengine_add_test(test_rt_agent_session_hitl_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_rt_agent_session_hitl_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- ADR-037 Phase 2 -- porting test_agent_session_real_backend(.cpp) (ADR-018: a REAL ChatClient
  # backend driven through a live AgentSession turn loop) and test_agent_session_skills_real_backend
  # (.cpp) (009 §8 + ADR-018: HistoryAndSkillsProvider through a real backend) onto
  # agentengine::rt::AgentSession directly, consolidated into ONE file (both share the identical
  # canned-local-HTTP-server shape) -- supersedes both. Still Windows-only (needs
  # agentengine::worktree_store's real compute_digest for the skills half) and still
  # AGENTENGINE_WITH_HTTPS-gated (guarded inside the .cpp itself, so this registration is
  # unconditional -- the file's own #else main(){return 0;} makes it a no-op test when HTTPS is off).
  if(WIN32)
    agentengine_add_test(test_rt_agent_session_real_backend
      LIBS agentengine::core agentengine::provider_http_client agentengine::worktree_store
        agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)

    # ---- Skills (009 §8), LIVE: the same HistoryAndSkillsProvider composition driven through a
    # two-turn conversation (greeting, then "use your code execution skill") against a REAL remote
    # model over OpenRouter -- the behavioural proof that a mounted skill's advertisement actually
    # reaches and influences a real model's real tool selection, not just the outbound wire bytes
    # (test_agent_session_skills_real_backend.cpp's own canned-server scope).
    # ADR-037: ported off quark::TestKit<Session> onto agentengine::rt::AgentSession directly --
    # supersedes the old test_agent_session_skills_live_e2e.cpp (removed).
    agentengine_add_test(test_rt_agent_session_skills_live_e2e
      LIBS agentengine::core agentengine::provider_http_client agentengine::worktree_store
        agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    set_tests_properties(test_rt_agent_session_skills_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

    # ---- Farm-field-advisory (009 §8 + ADR-024's Phase 3 addendum), LIVE: a NEW skill/tool pair
    # purpose-built for this file (farm-field-advisory -> check_field_weather), not a reuse of the
    # builtin five or of execute_code -- a rice-farmer scenario driven through the same
    # advertise -> mount_skill -> next-turn-unlocked-tool round-trip as the skills test above, against
    # a REAL remote model over OpenRouter. See the .cpp's own top comment for the full rationale.
    agentengine_add_test(test_rt_agent_session_farm_advisory_live_e2e
      LIBS agentengine::core agentengine::provider_http_client agentengine::worktree_store
        agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    set_tests_properties(test_rt_agent_session_farm_advisory_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

    # ---- Farm operations (009 §8 multi-skill + ADR-065 ToolOptimizerProvider + 010 real CPython
    # sandbox), LIVE: an expanded scenario -- two independent skills (crop-field-operations,
    # livestock-care-operations) gating their own domain tools via the same advertise->mount_skill->
    # next-turn-unlocked round-trip the two tests above already prove, PLUS a separate five-tool
    # scheduling/market pool gated through a REAL ToolOptimizerProvider (search_tools/mount_tool/
    # unmount_tool, ADR-065) so that mechanism has enough of a tool surface to be meaningful, PLUS one
    # genuinely real tool (execute_code) backed by the real embedded CPython interpreter
    # (native_jail::PythonRunner, not a canned stand-in) for actual numeric computation -- against a
    # REAL remote model over OpenRouter. Needs a real CPython install to link against, so this block is
    # further gated by AGENTENGINE_BUILD_PYTHON_RUNNER on top of the outer WIN32/AGENTENGINE_WITH_HTTPS
    # gating above (the .cpp's own #else main(){return 0;} still applies if HTTPS alone is off). See
    # the .cpp's own top comment for the full rationale.
    if(AGENTENGINE_BUILD_PYTHON_RUNNER)
      agentengine_add_test(test_rt_agent_session_farm_ops_live_e2e
        LIBS agentengine::core agentengine::provider_http_client agentengine::worktree_store
          agentengine::python_runner agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509
          MbedTLS::mbedcrypto
        DEFINITIONS
          AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
      set_tests_properties(test_rt_agent_session_farm_ops_live_e2e PROPERTIES
        # 480s, not the other live tests' 300s: this file's own max_turns is intentionally unbounded
        # (an ordinary open-ended conversation, not a fixed-step workflow -- see the .cpp's own
        # comment at the initialize() call site), so ctest's TIMEOUT is the only outer bound on a
        # session that occasionally needs several extra rounds to recover and converge.
        LABELS "live-network" TIMEOUT 480
        ENVIRONMENT "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}")
    endif()
  endif()
endif()

# ---- ADR-037 Phase 2 -- per-run TokenBudget<N> enforcement, ported onto rt::AgentSession directly.
# Claim (2) (over-budget fails closed) was already covered by test_rt_agent_session.cpp's S4 and
# test_rt_agent_session_streaming_and_events.cpp's A4b -- NOT re-ported; this file covers the three
# claims that were genuinely uncovered (under-budget succeeds, unbounded default, per-run reset).
agentengine_add_test(test_rt_agent_session_token_budget LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2 -- ADR-027's real, internal tool-call loop, ported onto rt::AgentSession
# directly -- supersedes test_agent_session_tool_call_loop.cpp.
agentengine_add_test(test_rt_agent_session_tool_call_loop LIBS agentengine::core agentengine_warnings)

# ---- Regression proof for the resolve_codeact_ask()/ask-pending max_turns_ bypass fix, found by a
# red-team round dedicated to session_builder.hpp's own "finding 7" (docs/planning/quickstart-session-
# builder-design-draft.md). No real MediatedPythonRunner needed -- see this file's own top comment.
agentengine_add_test(test_rt_agent_session_codeact_ask_max_turns LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md wired for real:
# AgentSession::set_turn_middleware_hook() -- the turn/pre_model mechanism proven standalone in
# test_turn_middleware.cpp now proven against a real AgentSession round.
agentengine_add_test(test_rt_agent_session_turn_middleware LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-069-content-triggered-model-response-replay.md wired for real: ContentReplayGateway
# plugs directly into AgentSession's existing ChatClientT slot (ModelCallGatewayLike), zero core changes.
agentengine_add_test(test_rt_agent_session_content_replay LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-066-context-provider-attribution-provenance.md wired for real: a composed
# multi-provider HistoryProviderT slot (already-existing composition path, ComposedContextProvider)
# carries attribution all the way to a real AgentSession round's outbound ChatRequest.
agentengine_add_test(test_rt_agent_session_context_provenance LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md wired for real:
# QuarantineToolProvider (trust/secret_quarantine.hpp) occupies AgentSession's HistoryProviderT slot;
# a real tool-call round reaches the quarantine store through the actual invoke_tool() pipeline.
# QuarantineSecretStore depends on trust::hmac_sha256, now the portable agentengine::hmac library
# (decisions/ADR-107-hmac-sha256-linux-parity.md) -- previously WIN32-only when hmac_sha256 was only
# reachable via agentengine::capability_token.
agentengine_add_test(test_rt_agent_session_quarantine_tool
  LIBS agentengine::core agentengine::hmac agentengine_warnings)

# ---- decisions/ADR-069-content-triggered-model-response-replay.md §7's own open question, answered
# with real evidence: ADR-067's turn/pre_model hook and ADR-069's ContentReplayGateway, both wired
# into ONE real AgentSession simultaneously -- proves they compose correctly, not merely independently.
agentengine_add_test(test_rt_agent_session_turn_and_replay_composition
  LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2, Slice 1 -- agentengine::rt::AgentSession, the Quark-actor-free replacement
# for agentengine::AgentSession's core turn loop.
agentengine_add_test(test_rt_agent_session LIBS agentengine::core agentengine_warnings)

# ---- Gap-16/21 fix (2026-08-14, decisions/ADR-042-*.md) -- ContextContribution::instructions
# actually reaching the model as a real role::system Message, taint-checked at the one explicit
# declassification site.
agentengine_add_test(test_rt_agent_session_instructions LIBS agentengine::core agentengine_warnings)

# ---- ADR-048 (gap-audit finding 19, Phase 1) -- the same gate, wired end-to-end into
# AgentSession::run_model_call(): a real run fails closed and chat() is never invoked.
agentengine_add_test(test_rt_agent_session_media_capability_gate LIBS agentengine::core agentengine_warnings)

# ---- ADR-049 (gap-audit finding 20 / 003 §8 Q2) -- cross-provider Reasoning exclusion, wired
# end-to-end into AgentSession::run_rounds().
agentengine_add_test(test_rt_agent_session_reasoning_provenance LIBS agentengine::core agentengine_warnings)

# ---- Gap-15 fix (2026-08-14, decisions/ADR-043-*.md) -- start_run_with_ack_policy()'s
# at_most_once/require_durable sequencing, and the turn-delta durable write/read round trip.
agentengine_add_test(test_rt_agent_session_ack_policy LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2, Slice 1 -- five real behavioral claims ported from the old, Quark-actor-based
# test_agent_session_state/test_agent_session_history_window/test_agent_session_fork/
# test_agent_session_redact/test_agent_session_interaction onto agentengine::rt::AgentSession
# directly (STATE/WIN/FORK/REDACT/INT) -- supersedes all five.
agentengine_add_test(test_rt_agent_session_lifecycle LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2, Slice 2 -- agentengine::rt::AgentSession's snapshot/checkpoint surface,
# built against rt::SessionStore instead of quark::FenceToken/snapshot_sequential.
agentengine_add_test(test_rt_agent_session_snapshot LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2, Slice 3 -- agentengine::rt::AgentSession's standing-effects/background-task
# surface, backed by a BackgroundCompletionQueue instead of quark::ActorRef::tell().
agentengine_add_test(test_rt_agent_session_background_task LIBS agentengine::core agentengine_warnings)

# ---- ADR-053 (gap-7 closure) -- agentengine::rt::AgentSession's THIRD real StandingEffect producer,
# schedule_wakeup()/due_standing_effects() (agent_session.hpp's "SLICE 4 ADDITION").
agentengine_add_test(test_rt_agent_session_schedule_wakeup LIBS agentengine::core agentengine_warnings)

# ---- ADR-053 §5 follow-up -- schedule_wakeup exposed as a real, model-callable declared tool.
agentengine_add_test(test_rt_agent_session_schedule_wakeup_tool LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2, Slice 1 -- ADR-029's suspend-for-human-approval mechanism, re-proven against
# agentengine::rt::AgentSession -- supersedes the old, Quark-actor-based
# test_agent_session_suspend_approval(.cpp).
agentengine_add_test(test_rt_agent_session_suspend_approval LIBS agentengine::core agentengine_warnings)

# ---- OQ-21 (core/tool_call_hook.hpp) prove phase -- the tool-call hook stage wired into a real
# rt::AgentSession round: deny-before-approval, rewrite-reaches-dispatch-and-downgrades-provenance,
# a no-hook regression/positive control, and the interaction_reason::hook_decision suspend/resume
# path. Pure AgentSession/tool_pipeline logic, no CPython -- matches
# test_rt_agent_session_suspend_approval's own link set exactly.
agentengine_add_test(test_rt_agent_session_tool_call_hook LIBS agentengine::core agentengine_warnings)

# ---- ADR-183: approval_resolved pairs with approval_requested and precedes tool_call_started, on
# the plain, hook-touched and cascade approval paths.
agentengine_add_test(test_rt_agent_session_approval_resolved_order
  LIBS agentengine::core agentengine_warnings)

# ---- ADR-044 prove phase -- EffectContext::report_progress(), a real call-scoped reverse channel
# into emit_run_event()/enable_event_stream() (run_event_kind::tool_call_delta), bracketed at all
# three real invoke_tool() call sites in rt/agent_session.hpp, plus a direct regression test against
# tool_pipeline.hpp::background_task() proving the §4 must-fix finding (a Backgroundable tool's
# invoke() must never reach a report_progress closure copied from a racing foreground bracket) is
# closed structurally. Pure AgentSession/tool_pipeline logic, no CPython -- belongs in the default
# `build` tree, matching test_rt_agent_session_suspend_approval's own link set exactly.
agentengine_add_test(test_agent_session_tool_call_progress LIBS agentengine::core agentengine_warnings)

# ---- ADR-042 (Design B) prove phase -- OutputSchema<T> wired into a live rt::AgentSession turn:
# set_output_schema()'s additive opt-in, native-only request-side scoping, and response-side
# validation at the convergence branch. Pure AgentSession/ChatClient logic, no CPython -- belongs in
# the default `build` tree, matching test_rt_agent_session_suspend_approval's own link set exactly
# (agentengine::core + agentengine_warnings only, no AGENTENGINE_BUILD_PYTHON_RUNNER gate needed).
agentengine_add_test(test_agent_session_output_schema LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2 -- porting test_session_scoped_stateful_tools(.cpp) (ADR-028's session-scoped
# stateful tools mechanism), test_codeact_session_isolation(.cpp) (ADR-030's full-stack session
# isolation proof), and test_agent_session_delegation(.cpp) (018 §1's on_behalf_of delegation) onto
# agentengine::rt::AgentSession directly -- supersedes all three. test_codeact_runner_binding.cpp
# (above) is untouched -- it proves CodeActRunnerBinding<RunnerT>'s own claim/fail-closed logic in
# isolation, with no AgentSession involvement to migrate.
agentengine_add_test(test_rt_agent_session_tooling_and_delegation LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2, Slice 1 -- ports test_agent_session_admission.cpp (018 §2 admission),
# test_agent_session_isolation.cpp (001 §1 per-session isolation, session_actor_id() restated as a
# direct behavioral proof since rt:: has no actor-id concept), test_agent_session_poison_run.cpp
# (019 §4 PoisonRunPolicy<N> quarantine, copied dependency-free rather than pulled in from the
# Quark-coupled core/agent_session.hpp), and test_agent_session_run_identity.cpp (001 §1/§2 run_id/
# turn_index identity, extended with a within-run turn_index-advances proof) onto rt::AgentSession
# directly -- supersedes all four.
agentengine_add_test(test_rt_agent_session_identity_and_admission LIBS agentengine::core agentengine_warnings)

# ADR-061 §20-§29 (Tier 3 per-request authority mechanism) -- real positive/negative controls against
# the compiled build, not just design text.
agentengine_add_test(test_rt_agent_session_tier3_authority LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2 -- porting test_agent_session_streaming_model_calls(.cpp) (ADR-034 streaming
# turn loop), test_agent_session_response_format_leak_scan(.cpp) (ADR-035 Phase 1 leak scan), and
# test_agent_session_run_event_stream(.cpp) (Milestone 7 Phase A run event stream) onto
# agentengine::rt::AgentSession directly -- supersedes all three.
agentengine_add_test(test_rt_agent_session_streaming_and_events LIBS agentengine::core agentengine_warnings)

if(AGENTENGINE_BUILD_PYTHON_RUNNER)
  set(AE_PYTHON_TEST_ENV "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}")

  # ---- ADR-041 (agent.ask suspend/replay, Design B) prove phase -----------------------------------
  # Needs a REAL rt::AgentSession<...> driving a REAL MediatedPythonRunner -- links both
  # agentengine::core (rt::AgentSession) and agentengine::mediated_python_runner, matching
  # test_mediated_python_runner_agent_tools' own link set plus the rt:: pieces
  # test_rt_agent_session_suspend_approval already needs.
  agentengine_add_test(test_agent_session_suspend_codeact_ask
    LIBS agentengine::core agentengine::mediated_python_runner agentengine_warnings
    DEFINITIONS
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  set_tests_properties(test_agent_session_suspend_codeact_ask PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
endif()

# ---- ADR-102 Phase 5 (agent_session.hpp fix) -- AgentSession::fork_from() now acquires
# source.session_mutex_ (I1) for the whole copy, closing a real, twice-independently-disclosed
# structural gap (ADR-102 Phase 3 SandboxRuntime::merge_into()/discard(), Phase 4's own residual naming
# fork_from() itself). Real two-thread race, not merely a re-run of the existing suite.
agentengine_add_test(test_rt_agent_session_fork_from_serialization
  LIBS agentengine::core agentengine_warnings)

# ADR-177: a session may re-issue a model call whose stream died mid-answer (opt-in, bounded per run).
add_executable(test_rt_agent_session_stream_retry test_rt_agent_session_stream_retry.cpp)
target_link_libraries(test_rt_agent_session_stream_retry PRIVATE agentengine::core agentengine_warnings)
# P13 adds a second AgentSession instantiation (an approval-gated tool table); under the ASan leg's
# instrumentation that pushed this one object past MSVC's section limit (C1128). A mechanical compiler
# limit, not a code defect: the same documented, scoped-to-one-target /bigobj as the targets above.
if(MSVC)
  target_compile_options(test_rt_agent_session_stream_retry PRIVATE /bigobj)
endif()
add_test(NAME test_rt_agent_session_stream_retry COMMAND test_rt_agent_session_stream_retry)

# ADR-178: AgentSession::cancel() -- cooperative, per-run, a canceled run is a state (run_canceled), not an abort.
agentengine_add_test(test_rt_agent_session_cancel LIBS agentengine::core agentengine_warnings)
