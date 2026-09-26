# tests/memory/: memory worktrees, MemoryProvider and procedural memory (029).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "memory" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- Milestone 4 Phase G1/G2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) --
# Memory worktree per principal (reusing A1's isolation one level up) + MemoryItem storage as blobs
# under {kind, id}. Real SHA-256 dependency, same as every other worktree-object-store test.
if(WIN32)
  agentengine_add_test(test_memory_worktree
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ---- Milestone 5 Phase I1 (018 §6, 029 §9 G4/G5) -- cross-TENANT memory isolation. Proves and
  # fixes a real bug: memory_ref_name()/memory_mount_id() (core/memory.hpp) derived from
  # principal.id alone through Milestone 4, so two principals with the same id in different tenants
  # collided on the identical memory worktree.
  agentengine_add_test(test_memory_cross_tenant_isolation
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Milestone 4 Phase G3/G4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) --
# MemoryProvider: default ranked retrieval + recall(query) tool (G4), extraction via a mock
# ChatClient (G3), composed with HistoryProvider through Phase B3's own deferred assemble_context().
if(WIN32)
  agentengine_add_test(test_memory_provider
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-190: the request both declared summarizers receive, and what a memory reply must look like to be stored.
  agentengine_add_test(test_summarizer_prompt
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-179 stage 0: concurrent writers to one principal's memory ref must not lose items.
  agentengine_add_test(test_memory_write_concurrency
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 E21 precondition: does a seeded lesson stay in the recalled top-N as episodic items accrue?
  agentengine_add_test(test_memory_lesson_recall
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-194: which channel a procedural memory item reaches the model by (029 §10 Q2's route).
  agentengine_add_test(test_memory_procedural_channel
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Milestone 4 Phase G5 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# 029 §9 G3 "no authority laundering": hostile ModelInferred memory content smuggled into a tool
# call's own arguments cannot auto-approve anything -- reuses ADR-009's capability/approval
# pipeline and 003 §2's taint model directly, no new predicate engine.
if(WIN32)
  agentengine_add_test(test_memory_no_authority_laundering
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- Milestone 4 Phase H3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Roadmap exit criterion (029 §9 G1): the default (non-vector) memory retrieval path is
# deterministic and replayable -- byte-identical ContextContribution across repeated on_context()
# calls against a fixed memory-worktree tree digest and a fixed turn, with no summarizer/network
# call ever occurring during retrieval.
if(WIN32)
  agentengine_add_test(test_memory_retrieval_determinism
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- ADR-047 (docs/planning/2026-08-10-full-codebase-adr-gap-audit.md gap 18) -----------------
# Proves the real product ranking formula (salience x recency x keyword) directly against
# memory_rank_score() -- recency actually participates, is bounded (not an unbounded raw SeqNo),
# and a zero-salience freshly-extracted item is not permanently rank-zero.
if(WIN32)
  agentengine_add_test(test_memory_ranking_formula
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

if(AGENTENGINE_WITH_HTTPS)
  # ADR-179 §1 / ADR-195 precondition: is a PLAUSIBLE learned lesson still followed by the safe route?
  # Live, observation only (see the file's own top comment); self-skips without a key.
  agentengine_add_test(test_memory_lesson_effectiveness_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_memory_lesson_effectiveness_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 900)

  # Why a real model does not act on a promoted lesson when it must choose a TOOL ARGUMENT: separates the
  # memory label from the ADR-173 fence (ADR-194's design), for the lessons test_eval_tier1_screen_live_e2e
  # found inert. Every call is logged to a JSONL file and the table is computed from the file. Observation only.
  agentengine_add_test(test_memory_lesson_label_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine::worktree_store
      agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_memory_lesson_label_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 3600)

  # ADR-194 (wire half): what BOTH real serializers do with the two shapes the routes produce.
  agentengine_add_test(test_memory_procedural_wire
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings)

  # ADR-194: a live measurement of what a real model does with a hostile learned instruction
  # delivered by each route (observation only; see the file's own top comment).
  agentengine_add_test(test_memory_procedural_channel_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_memory_procedural_channel_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)
endif()

# ---- decisions/ADR-153-agent-memory-codeact-bridging.md -- src/backends/native_jail/
# memory_mount_materializer.hpp (the same materialize_mount shape as skill_mount_materializer, narrowed
# to one principal's own memory mount) plus MemoryProvider::make_recall_tool_descriptor() now public and
# proven through the real bridge_tool_call() path (tool_bridge.hpp), not merely through
# ContextContribution.tools the way test_memory_provider.cpp already covers it.
if(WIN32)
  add_executable(test_memory_codeact_bridging test_memory_codeact_bridging.cpp)
  target_include_directories(test_memory_codeact_bridging PRIVATE "${AE_TESTS_SOURCE_DIR}/../src")
  target_link_libraries(test_memory_codeact_bridging PRIVATE
    agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_codeact_bridging COMMAND test_memory_codeact_bridging)
  set_tests_properties(test_memory_codeact_bridging PROPERTIES TIMEOUT 60)
endif()

# ---- decisions/ADR-156-agent-notes-write-back.md -- src/backends/native_jail/
# memory_notes_materializer.hpp: a dedicated notes-inbox subtree (isolated from write_memory_item()'s
# own structural <kind>/<id> storage) plus harvest_memory_notes(), converting a script's real file
# writes into durable AgentAuthored MemoryItems, discoverable through the SAME rank_memory_items()/
# recall path other memory items use.
if(WIN32)
  add_executable(test_memory_notes_write_back test_memory_notes_write_back.cpp)
  target_include_directories(test_memory_notes_write_back PRIVATE "${AE_TESTS_SOURCE_DIR}/../src")
  target_link_libraries(test_memory_notes_write_back PRIVATE
    agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_notes_write_back COMMAND test_memory_notes_write_back)
  set_tests_properties(test_memory_notes_write_back PROPERTIES TIMEOUT 60)
endif()
