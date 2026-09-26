# tests/core/context/: context providers, context assembly, middleware and the session builder (005).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/context" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- Milestone 4 Phase B2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# HistoryProvider<Window<N>>, the first real ContextProvider conformer -- standalone (unit) and
# wired end to end through AgentSession's real turn loop.
agentengine_add_test(test_history_provider LIBS agentengine::core agentengine_warnings)

# ---- Milestone 4 Phase B3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Generalized N-contributor assemble_context(): per-contributor token budgets, deterministic
# oldest-first drop order, drops recorded, purity/replay across repeated runs.
agentengine_add_test(test_context_assembly LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-066-context-provider-attribution-provenance.md's prove phase: per-contributor
# attribution stamping on Message/ToolDescriptor, and the narrower (not blanket) content_origin::user
# forgery check, both applied structurally at assemble_context()'s own seam.
agentengine_add_test(test_context_provenance LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md's prove phase: the turn/
# pre_model interception point (TurnContext, ToolSurfaceView, redact_subspan(), Compactor<N>), wired
# as its own standalone, tested mechanism -- not yet threaded into rt::AgentSession::run_rounds()
# (named residual, matching middleware.hpp's own precedent for the run/tool-call points).
agentengine_add_test(test_turn_middleware LIBS agentengine::core agentengine_warnings)

# ---- ComposedContextProvider<Ms...> (core/composed_context_provider.hpp) -- generalizes
# history_and_skills_provider.hpp's fixed two-provider HistoryAndSkillsProvider<H,S> to an arbitrary
# Ms... pack via the SAME assemble_context() B3 already proved, and wires it into rt::AgentSession's
# single provider template slot end to end (N == 3 providers, deliberately > 2).
agentengine_add_test(test_composed_context_provider LIBS agentengine::core agentengine_warnings)

# ---- Milestone 4 Phase B4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# HistoryProvider<Summarize<N, SummarizerT>> -- the second named compaction strategy (005 §4),
# proven against a mock summarizer ChatClient (decision 8's established precedent).
agentengine_add_test(test_history_provider_summarize LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-166-todo-provider.md, issue #53: TodoProvider, a ContextProvider conformer for
# plan/execute state (005 §5). Adaptive contribution, bounded item count/title length, unknown-id
# rejection, and composition with a second real ContextProvider through the shared assemble_context().
# No worktree dependency (pure in-memory, no persistence) -- unlike MemoryProvider, not WIN32-gated.
agentengine_add_test(test_todo_provider LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-167-plan-execute-mode.md, issue #54: PlanExecuteMode, a ContextProvider + a
# PolicyDecider factory gating a single agent into "plan first, then execute." Depends on
# TodoProvider's shared TodoState (todo_provider.hpp) for its plan evidence.
agentengine_add_test(test_plan_execute_mode LIBS agentengine::core agentengine_warnings)

if(AGENTENGINE_WITH_HTTPS)
  # ---- QuickstartSessionBuilder (core/session_builder.hpp), promoted from prototype after three
  # red-team rounds -- see docs/planning/quickstart-session-builder-design-draft.md. Covers build()-time
  # capability/secret validation and the Bundle Store/CapabilitySet lifetime-across-a-move proof. No
  # .ask()/start_run() call, so no real network exchange happens and OpenAIChatClient::chat()'s body is
  # never instantiated -- linked against provider_http_client only because session_builder.hpp's own
  # file-wide AGENTENGINE_WITH_HTTPS guard requires the real OpenAIChatClient/AnthropicChatClient types
  # to be visible at all, same reason test_json_schema_described above needs the same link with no live
  # network call either. Also links agentengine::hmac (decisions/ADR-107-hmac-sha256-linux-parity.md):
  # B7 below builds a real session against QuarantineSecretStore (trust/secret_quarantine.hpp), whose
  # quarantine() ODR-uses trust::hmac_sha256 -- a real, pre-existing missing-link-dependency bug this
  # target carried on EVERY platform (not just Linux), only ever masked because nothing had linked
  # AGENTENGINE_WITH_HTTPS on for a from-scratch Linux configure before ADR-105/106 to surface it, and
  # apparently never exercised as a from-scratch relink on Windows either; fixed here, not merely
  # disclosed, since it is a one-line addition with no design question attached.
  agentengine_add_test(test_session_builder
    LIBS agentengine::core agentengine::provider_http_client agentengine::hmac agentengine_warnings)

  # ---- unified-streaming-design-draft.md §4 (Piece D), live proof: Bundle::ask()/ask_stream() against
  # REAL OpenRouter -- the one surface this facade's own history names as out of scope until now ("a
  # live, real-network proof of .ask() end to end is separately scoped future work"). Needs the real
  # MbedTLS-backed HTTPS stack, unlike its offline sibling above.
  agentengine_add_test(test_session_builder_openrouter_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_session_builder_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)
endif()

# ---- issue #15 / docs/planning/tool-optimizer-provider-design-draft.md / ADR-065: ToolOptimizerProvider
# (core/tool_optimizer_provider.hpp) -- an ordinary composite ContextProvider gating MCP/plugin/agent
# tool exposure the same way mount_skill already gates skill tools. Core-tier only (no native_jail/
# Python dependency) -- proves the declare/invoke cadence through the real invoke_tool pipeline (R2,
# mirroring test_on_demand_skill_mount.cpp's own R2), cross-source/reserved-name collision rejection
# (R3), search_tools (R4), a structural no-leakage check (R5), and the second real (non-fixture)
# 3-provider ComposedContextProvider production composition in the tree, wired into a real
# rt::AgentSession (R6).
if(WIN32)
  agentengine_add_test(test_tool_optimizer_provider
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()
