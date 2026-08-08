# Correctness gate (CONVENTIONS.md: "tests/ — correctness gate, one per load-bearing invariant").
# smoke_vocabulary proves the abstract-vocabulary headers under include/agentengine/ actually
# compile together and that the declared concepts (ChatClient, Runner, SandboxBackend) are
# satisfiable by a conforming type.

# ---- 007 §9 G2 compile-fail proof (docs/planning/milestone-1-core-substrate-breakdown.md task 2) --
# I3's static half: Tainted<T> must have NO implicit conversion to the untainted capability-API
# surface. A configure-time try_compile() gate, not a CTest entry — try_compile() runs during
# `cmake -S . -B build` itself, and a FATAL_ERROR here stops configure exactly as reliably as a
# failing test stops `ctest` (CI always runs configure before ctest); this is CMake's own standard
# idiom for a compile-fail proof, chosen over inventing a custom ctest-time try_compile wrapper for
# the same effect. Two files, not one: a fail-only suite can't distinguish "correctly rejected" from
# "nothing here compiles at all," so tainted_declassify_positive_control.cpp proves the explicit
# unsafe_view() declassifier still compiles.
try_compile(AE_TAINTED_REJECTS_IMPLICIT_CONVERSION
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/no_implicit_conversion"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/tainted_no_implicit_conversion.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_TAINTED_REJECTS_IMPLICIT_CONVERSION_LOG)
if(AE_TAINTED_REJECTS_IMPLICIT_CONVERSION)
  message(FATAL_ERROR
    "007 §9 G2 VIOLATED: tests/compile_fail/tainted_no_implicit_conversion.cpp compiled, but "
    "Tainted<T> must have NO implicit conversion to an untainted capability-API parameter type "
    "(I3's static half). Build log:\n${AE_TAINTED_REJECTS_IMPLICIT_CONVERSION_LOG}")
endif()

try_compile(AE_TAINTED_DECLASSIFY_POSITIVE_CONTROL
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/declassify_positive_control"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/tainted_declassify_positive_control.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_TAINTED_DECLASSIFY_POSITIVE_CONTROL_LOG)
if(NOT AE_TAINTED_DECLASSIFY_POSITIVE_CONTROL)
  message(FATAL_ERROR
    "007 §9 G2 positive control FAILED: tests/compile_fail/tainted_declassify_positive_control.cpp "
    "must compile (the explicit unsafe_view() declassifier is a legitimate, intended path) but did "
    "not. Build log:\n${AE_TAINTED_DECLASSIFY_POSITIVE_CONTROL_LOG}")
endif()
message(STATUS
  "007 §9 G2 compile-fail proof: OK (Tainted<T> rejects implicit conversion; explicit "
  "declassification compiles)")

# ---- 007 §9 G1 / ADR-009 compile-fail proof (M2 Phase A task A4) -------------------------------
# Miniature "no ambient authority" claim, one level below A4's original full scope (a tool/agent
# declaration site doesn't exist yet -- that's Phase B/E): CapabilitySet itself has no construction
# path except grant_root(). Same try_compile() idiom as the 007 §9 G2 gate above.
try_compile(AE_CAPABILITY_SET_REJECTS_DIRECT_CONSTRUCTION
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/capability_set_no_direct_construction"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/capability_set_no_direct_construction.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_CAPABILITY_SET_REJECTS_DIRECT_CONSTRUCTION_LOG)
if(AE_CAPABILITY_SET_REJECTS_DIRECT_CONSTRUCTION)
  message(FATAL_ERROR
    "007 §9 G1 VIOLATED: tests/compile_fail/capability_set_no_direct_construction.cpp compiled, "
    "but CapabilitySet must have NO construction path except grant_root() (no ambient-authority "
    "shortcut). Build log:\n${AE_CAPABILITY_SET_REJECTS_DIRECT_CONSTRUCTION_LOG}")
endif()

try_compile(AE_CAPABILITY_SET_GRANT_ROOT_POSITIVE_CONTROL
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/capability_set_grant_root_positive_control"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/capability_set_grant_root_positive_control.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_CAPABILITY_SET_GRANT_ROOT_POSITIVE_CONTROL_LOG)
if(NOT AE_CAPABILITY_SET_GRANT_ROOT_POSITIVE_CONTROL)
  message(FATAL_ERROR
    "007 §9 G1 positive control FAILED: tests/compile_fail/capability_set_grant_root_positive_control.cpp "
    "must compile (grant_root() is the intended, legitimate construction path) but did not. "
    "Build log:\n${AE_CAPABILITY_SET_GRANT_ROOT_POSITIVE_CONTROL_LOG}")
endif()
message(STATUS
  "007 §9 G1 compile-fail proof: OK (CapabilitySet rejects direct construction; grant_root() compiles)")

# ---- ADR-012 compile-fail proof (SandboxProfile<P>'s template-parameter kind) -------------------
# Same try_compile() idiom as the two gates above: `SandboxProfile<P>` (core/agent.hpp) constrains
# `P` via `SandboxProfileArg` (sandbox/sandbox.hpp) -- a real `SandboxBackend`, or the `Strict`
# resolution selector. A non-conforming `P` (e.g. `int`) must fail to compile at the declaration
# site, not silently accept a nonsense type.
try_compile(AE_SANDBOX_PROFILE_REJECTS_NON_CONFORMING_TYPE
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/sandbox_profile_rejects_non_conforming_type"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/sandbox_profile_rejects_non_conforming_type.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/quark/include;${PROJECT_SOURCE_DIR}/third_party/quark"
  OUTPUT_VARIABLE AE_SANDBOX_PROFILE_REJECTS_NON_CONFORMING_TYPE_LOG)
if(AE_SANDBOX_PROFILE_REJECTS_NON_CONFORMING_TYPE)
  message(FATAL_ERROR
    "ADR-012 VIOLATED: tests/compile_fail/sandbox_profile_rejects_non_conforming_type.cpp compiled, "
    "but SandboxProfile<P> must reject a P that satisfies neither SandboxBackend nor Strict. "
    "Build log:\n${AE_SANDBOX_PROFILE_REJECTS_NON_CONFORMING_TYPE_LOG}")
endif()

try_compile(AE_SANDBOX_PROFILE_POSITIVE_CONTROL
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/sandbox_profile_positive_control"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/sandbox_profile_positive_control.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/quark/include;${PROJECT_SOURCE_DIR}/third_party/quark"
  OUTPUT_VARIABLE AE_SANDBOX_PROFILE_POSITIVE_CONTROL_LOG)
if(NOT AE_SANDBOX_PROFILE_POSITIVE_CONTROL)
  message(FATAL_ERROR
    "ADR-012 positive control FAILED: tests/compile_fail/sandbox_profile_positive_control.cpp must "
    "compile (Strict and a real conforming SandboxBackend are both legitimate SandboxProfile<P> "
    "arguments) but did not. Build log:\n${AE_SANDBOX_PROFILE_POSITIVE_CONTROL_LOG}")
endif()
message(STATUS
  "ADR-012 compile-fail proof: OK (SandboxProfile<P> rejects a non-conforming type; Strict and a "
  "real SandboxBackend both compile)")

# ---- 014 §1 compile-fail proof (Milestone 6 Phase A) -------------------------------------------
# "An edge that connects incompatible types fails to build -- at compile time for the C++ form."
# That claim is about a COMPILE failure, so it cannot be asserted from inside a test that must
# itself compile; try_compile() is the only honest form it can take. Same two-file idiom as the
# gates above -- a fail-only check cannot distinguish "correctly rejected" from "this file never
# compiled for an unrelated reason", so the positive control connects a MATCHING pair.
try_compile(AE_WORKFLOW_REJECTS_EDGE_TYPE_MISMATCH
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/workflow_edge_type_mismatch"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/workflow_edge_type_mismatch.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_WORKFLOW_REJECTS_EDGE_TYPE_MISMATCH_LOG)
if(AE_WORKFLOW_REJECTS_EDGE_TYPE_MISMATCH)
  message(FATAL_ERROR
    "014 §1 VIOLATED: tests/compile_fail/workflow_edge_type_mismatch.cpp compiled, but an edge "
    "whose source output message type is not its target input message type must fail to build in "
    "the C++ form. Build log:\n${AE_WORKFLOW_REJECTS_EDGE_TYPE_MISMATCH_LOG}")
endif()

try_compile(AE_WORKFLOW_EDGE_TYPE_MATCH_POSITIVE_CONTROL
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/workflow_edge_type_match"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/workflow_edge_type_match_positive_control.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_WORKFLOW_EDGE_TYPE_MATCH_POSITIVE_CONTROL_LOG)
if(NOT AE_WORKFLOW_EDGE_TYPE_MATCH_POSITIVE_CONTROL)
  message(FATAL_ERROR
    "014 §1 positive control FAILED: tests/compile_fail/workflow_edge_type_match_positive_control.cpp "
    "must compile (a type-compatible edge is the ordinary, intended case) but did not. Build "
    "log:\n${AE_WORKFLOW_EDGE_TYPE_MATCH_POSITIVE_CONTROL_LOG}")
endif()
message(STATUS
  "014 §1 compile-fail proof: OK (a type-mismatched edge fails to build; a matching edge compiles)")

# ---- Milestone 3 Phase A3 compile-fail proof (025 §2's Blob/Tree immutability) ------------------
# `InMemoryWorktreeObjectStore::put_blob`/`put_tree` (core/worktree.hpp) take ONLY the content --
# the digest is always DERIVED from it, never accepted as a caller-supplied parameter -- which is
# what makes "immutable bytes, addressed by digest" true by construction. A two-argument
# put_blob(digest, bytes)/put_tree(digest, tree) overload would reopen exactly that hole, so
# calling either shape must fail to compile. WIN32-gated: all three checks link the real
# compute_digest implementation (src/core/worktree_digest.cpp + bcrypt) alongside themselves --
# deliberately, even the two that are EXPECTED to fail. Without that, a negative control could stay
# "green" (fail to build) forever for the wrong reason -- a permanently missing link dependency --
# and never actually re-test whether the forbidden overload exists once it did compile; this way,
# the only thing standing between "fails" and "builds clean" is the overload itself, matching the
# positive control's own build shape exactly (confirmed load-bearing below, not asserted).
if(WIN32)
  try_compile(AE_WORKTREE_STORE_REJECTS_BLOB_DIGEST_OVERRIDE
    "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/worktree_store_rejects_blob_digest_override"
    SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/worktree_object_store_rejects_blob_digest_override.cpp"
      "${PROJECT_SOURCE_DIR}/src/core/worktree_digest.cpp"
    CMAKE_FLAGS
      "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/quark/include;${PROJECT_SOURCE_DIR}/third_party/quark"
      "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    LINK_LIBRARIES bcrypt
    OUTPUT_VARIABLE AE_WORKTREE_STORE_REJECTS_BLOB_DIGEST_OVERRIDE_LOG)
  if(AE_WORKTREE_STORE_REJECTS_BLOB_DIGEST_OVERRIDE)
    message(FATAL_ERROR
      "Milestone 3 A3 VIOLATED: tests/compile_fail/worktree_object_store_rejects_blob_digest_override.cpp "
      "compiled, but put_blob must have NO two-argument (digest, bytes) overload -- a Blob's digest "
      "must always be derived from its content, never caller-supplied. "
      "Build log:\n${AE_WORKTREE_STORE_REJECTS_BLOB_DIGEST_OVERRIDE_LOG}")
  endif()

  try_compile(AE_WORKTREE_STORE_REJECTS_TREE_DIGEST_OVERRIDE
    "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/worktree_store_rejects_tree_digest_override"
    SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/worktree_object_store_rejects_tree_digest_override.cpp"
      "${PROJECT_SOURCE_DIR}/src/core/worktree_digest.cpp"
    CMAKE_FLAGS
      "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/quark/include;${PROJECT_SOURCE_DIR}/third_party/quark"
      "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    LINK_LIBRARIES bcrypt
    OUTPUT_VARIABLE AE_WORKTREE_STORE_REJECTS_TREE_DIGEST_OVERRIDE_LOG)
  if(AE_WORKTREE_STORE_REJECTS_TREE_DIGEST_OVERRIDE)
    message(FATAL_ERROR
      "Milestone 3 A3 VIOLATED: tests/compile_fail/worktree_object_store_rejects_tree_digest_override.cpp "
      "compiled, but put_tree must have NO two-argument (digest, tree) overload -- a Tree's digest "
      "must always be derived from its own canonical serialization, never caller-supplied. "
      "Build log:\n${AE_WORKTREE_STORE_REJECTS_TREE_DIGEST_OVERRIDE_LOG}")
  endif()

  # try_compile() runs in an isolated mini-project that cannot see this build's own targets (no
  # ALIAS/IMPORTED target named agentengine::worktree_store exists there) -- so the positive
  # control compiles worktree_digest.cpp's compute_digest implementation alongside itself and links
  # the real system library (bcrypt, a plain linker name, not a CMake target) directly, rather than
  # depending on a target that only exists in the outer project.
  # Multi-source try_compile() ("srcfile..." with more than one entry) doesn't accept CXX_STANDARD
  # (CMake: "CXX_STANDARD allowed only in source file signature") -- set it via CMAKE_FLAGS instead.
  try_compile(AE_WORKTREE_STORE_POSITIVE_CONTROL
    "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/worktree_store_positive_control"
    SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/worktree_object_store_positive_control.cpp"
      "${PROJECT_SOURCE_DIR}/src/core/worktree_digest.cpp"
    CMAKE_FLAGS
      "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/quark/include;${PROJECT_SOURCE_DIR}/third_party/quark"
      "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    LINK_LIBRARIES bcrypt
    OUTPUT_VARIABLE AE_WORKTREE_STORE_POSITIVE_CONTROL_LOG)
  if(NOT AE_WORKTREE_STORE_POSITIVE_CONTROL)
    message(FATAL_ERROR
      "Milestone 3 A3 positive control FAILED: tests/compile_fail/worktree_object_store_positive_control.cpp "
      "must compile and link (the real, one-argument put_blob/put_tree signatures are the intended, "
      "legitimate path) but did not. Build log:\n${AE_WORKTREE_STORE_POSITIVE_CONTROL_LOG}")
  endif()
  message(STATUS
    "Milestone 3 A3 compile-fail proof: OK (put_blob/put_tree reject a caller-supplied digest; the "
    "real one-argument signatures compile)")
endif()

add_executable(smoke_vocabulary smoke_vocabulary.cpp)
target_link_libraries(smoke_vocabulary PRIVATE agentengine::core agentengine_warnings)

add_test(NAME smoke_vocabulary COMMAND smoke_vocabulary)

# ---- Milestone 1 exit criterion (docs/planning/milestone-1-core-substrate-breakdown.md task 4) --
# One hard-coded agent, one turn, against a mock ChatClient, on a real Quark AgentSession actor via
# quark::TestKit (single-actor harness, no cluster/engine bring-up needed).
add_executable(test_m1_walking_skeleton test_m1_walking_skeleton.cpp)
target_link_libraries(test_m1_walking_skeleton PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_m1_walking_skeleton COMMAND test_m1_walking_skeleton)

# ---- Milestone 4 Phase A1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# session_actor_id() + real two-session isolation, closing 001 §1's session_id-keying gap M1 left
# unwired.
add_executable(test_agent_session_isolation test_agent_session_isolation.cpp)
target_link_libraries(test_agent_session_isolation PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_isolation COMMAND test_agent_session_isolation)

# ---- Milestone 4 Phase A2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Typed `state` (005 §8 Q1) + `metadata` fields on AgentSession, neither of which existed before.
add_executable(test_agent_session_state test_agent_session_state.cpp)
target_link_libraries(test_agent_session_state PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_state COMMAND test_agent_session_state)

# ---- Milestone 4 Phase A3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Real 001 §1/§2 Run/Turn identity (run_id/turn_index on EffectContext), minted per StartRun.
add_executable(test_agent_session_run_identity test_agent_session_run_identity.cpp)
target_link_libraries(test_agent_session_run_identity PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_run_identity COMMAND test_agent_session_run_identity)

# ---- Milestone 4 Phase A4, narrowed (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) --
# session_id/principal/created_at/updated_at through Quark's real Store via the Snapshot model.
# history[]/state/metadata explicitly NOT covered (Message/ContentItem have no QUARK_SERIALIZE yet).
add_executable(test_agent_session_snapshot test_agent_session_snapshot.cpp)
target_link_libraries(test_agent_session_snapshot PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_snapshot COMMAND test_agent_session_snapshot)

# ---- Milestone 4 Phase B2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# HistoryProvider<Window<N>>, the first real ContextProvider conformer -- standalone (unit) and
# wired end to end through AgentSession's real turn loop.
add_executable(test_history_provider test_history_provider.cpp)
target_link_libraries(test_history_provider PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_history_provider COMMAND test_history_provider)

add_executable(test_agent_session_history_window test_agent_session_history_window.cpp)
target_link_libraries(test_agent_session_history_window PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_history_window COMMAND test_agent_session_history_window)

# ---- Milestone 4 Phase B3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Generalized N-contributor assemble_context(): per-contributor token budgets, deterministic
# oldest-first drop order, drops recorded, purity/replay across repeated runs.
add_executable(test_context_assembly test_context_assembly.cpp)
target_link_libraries(test_context_assembly PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_context_assembly COMMAND test_context_assembly)

# ---- Milestone 4 Phase B4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# HistoryProvider<Summarize<N, SummarizerT>> -- the second named compaction strategy (005 §4),
# proven against a mock summarizer ChatClient (decision 8's established precedent).
add_executable(test_history_provider_summarize test_history_provider_summarize.cpp)
target_link_libraries(test_history_provider_summarize PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_history_provider_summarize COMMAND test_history_provider_summarize)

# ---- Milestone 4 Phase C1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# fork_from(): copy-on-write new session from a history prefix, reusing A1/A4 directly.
add_executable(test_agent_session_fork test_agent_session_fork.cpp)
target_link_libraries(test_agent_session_fork PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_fork COMMAND test_agent_session_fork)

# ---- Milestone 4 Phase C2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# redact(): in-place tombstone (Custom content kind) carrying reason/actor; proves propagation
# into a subsequently-computed Summarize<N> derived summary (005 §6).
add_executable(test_agent_session_redact test_agent_session_redact.cpp)
target_link_libraries(test_agent_session_redact PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_redact COMMAND test_agent_session_redact)

# ---- Milestone 4 Phase C3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# delete_session(): durable tombstone + in-process state clear, with a completion receipt.
add_executable(test_agent_session_delete test_agent_session_delete.cpp)
target_link_libraries(test_agent_session_delete PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_delete COMMAND test_agent_session_delete)

# ---- Milestone 4 Phase D1/D2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) --
# Checkpoint content (run position survives restart without reminting a used run_id) and cadence
# (CheckpointCadence<N>/checkpoint_if_due() actually skip writes between checks).
add_executable(test_agent_session_checkpoint test_agent_session_checkpoint.cpp)
target_link_libraries(test_agent_session_checkpoint PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_checkpoint COMMAND test_agent_session_checkpoint)

# ---- Milestone 4 Phase E1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Interaction (001 §2): mint/resolve lifecycle + checkpoint round-trip (closes D1's placeholder gap).
add_executable(test_agent_session_interaction test_agent_session_interaction.cpp)
target_link_libraries(test_agent_session_interaction PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_interaction COMMAND test_agent_session_interaction)

# ---- Milestone 4 Phase E2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Suspended holds zero resources -- the first AgentEngine test anywhere to stand up a real
# quark::Engine (every prior test uses quark::TestKit, which has no passivation to census).
add_executable(test_agent_session_suspend_resume test_agent_session_suspend_resume.cpp)
target_link_libraries(test_agent_session_suspend_resume PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_suspend_resume COMMAND test_agent_session_suspend_resume)

# ---- Milestone 4 Phase E3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Timer/schedule wake -- a real Quark durable reminder targets a session's real session_actor_id()
# and reaches it through a live Engine via TimerWake (pure reuse, decision 4).
add_executable(test_agent_session_timer_wake test_agent_session_timer_wake.cpp)
target_link_libraries(test_agent_session_timer_wake PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_timer_wake COMMAND test_agent_session_timer_wake)

# ---- Milestone 4 Phase E4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Node-loss fencing (reuse) -- a zombie activation's checkpoint write is rejected through
# AgentEngine's own save_agent_session_snapshot path.
add_executable(test_agent_session_node_loss_fencing test_agent_session_node_loss_fencing.cpp)
target_link_libraries(test_agent_session_node_loss_fencing PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_node_loss_fencing COMMAND test_agent_session_node_loss_fencing)

# ---- Milestone 4 Phase E4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Poison-run quarantine: PoisonRunPolicy<N> fires at exactly the bound; state preserved, not discarded.
add_executable(test_agent_session_poison_run test_agent_session_poison_run.cpp)
target_link_libraries(test_agent_session_poison_run PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_poison_run COMMAND test_agent_session_poison_run)

# ---- Milestone 6 Phase F prerequisite (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md) ----
# Message/ContentItem's durable projection (content_record.hpp) -- closes the "no QUARK_SERIALIZE"
# gap named at agent_session.hpp:154-158/207-213, needed for real by 014 §5's "resume restores
# exactly". See content_record.hpp's own header comment for the two rejected designs (a branching
# quark_describe breaks FingerprintFolder's determinism and the write pass's const-cast convention)
# this flat-Record-family design avoids.
add_executable(test_content_record_roundtrip test_content_record_roundtrip.cpp)
target_link_libraries(test_content_record_roundtrip PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_content_record_roundtrip COMMAND test_content_record_roundtrip)

# ---- Milestone 4 Phase F1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Idempotency key derivation {run_id, turn_index, call_index, argument_digest}, wired into the
# real (M2) tool_pipeline.hpp invocation pipeline.
add_executable(test_idempotency_key test_idempotency_key.cpp)
target_link_libraries(test_idempotency_key PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_idempotency_key COMMAND test_idempotency_key)

# ---- Milestone 4 Phase F2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Effect journaling (intent before execution, outcome after) atop Quark's Store/EventLog, reusing
# a session's own ActorId (session_actor_id, A1) -- no second persistence engine.
add_executable(test_effect_journal test_effect_journal.cpp)
target_link_libraries(test_effect_journal PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_effect_journal COMMAND test_effect_journal)

# ---- Milestone 4 Phase F4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# 019 §6's rewind-then-reexecute rule: effect_class/EffectClass<C>, authorize_reexecution(), proven
# against a real manually-triggered invoke_tool() re-execution under the same idempotency key.
add_executable(test_effect_reexecution test_effect_reexecution.cpp)
target_link_libraries(test_effect_reexecution PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_effect_reexecution COMMAND test_effect_reexecution)

# ---- Milestone 4 Phase G1/G2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) --
# Memory worktree per principal (reusing A1's isolation one level up) + MemoryItem storage as blobs
# under {kind, id}. Real SHA-256 dependency, same as every other worktree-object-store test.
if(WIN32)
  add_executable(test_memory_worktree test_memory_worktree.cpp)
  target_link_libraries(test_memory_worktree PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_worktree COMMAND test_memory_worktree)

  # ---- Milestone 5 Phase I1 (018 §6, 029 §9 G4/G5) -- cross-TENANT memory isolation. Proves and
  # fixes a real bug: memory_ref_name()/memory_mount_id() (core/memory.hpp) derived from
  # principal.id alone through Milestone 4, so two principals with the same id in different tenants
  # collided on the identical memory worktree.
  add_executable(test_memory_cross_tenant_isolation test_memory_cross_tenant_isolation.cpp)
  target_link_libraries(test_memory_cross_tenant_isolation PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_cross_tenant_isolation COMMAND test_memory_cross_tenant_isolation)
endif()

# ---- Milestone 4 Phase G3/G4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) --
# MemoryProvider: default ranked retrieval + recall(query) tool (G4), extraction via a mock
# ChatClient (G3), composed with HistoryProvider through Phase B3's own deferred assemble_context().
if(WIN32)
  add_executable(test_memory_provider test_memory_provider.cpp)
  target_link_libraries(test_memory_provider PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_provider COMMAND test_memory_provider)
endif()

# ---- Milestone 4 Phase G5 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# 029 §9 G3 "no authority laundering": hostile ModelInferred memory content smuggled into a tool
# call's own arguments cannot auto-approve anything -- reuses ADR-009's capability/approval
# pipeline and 003 §2's taint model directly, no new predicate engine.
if(WIN32)
  add_executable(test_memory_no_authority_laundering test_memory_no_authority_laundering.cpp)
  target_link_libraries(test_memory_no_authority_laundering PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_no_authority_laundering COMMAND test_memory_no_authority_laundering)
endif()

# ---- Milestone 4 Phase H1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Roadmap exit criterion (001 §9 G2 / 019 §7 G1): a session survives process restart with an
# identical resumed run -- simulated kill -9 at a checkpoint boundary vs. an uninterrupted control.
add_executable(test_session_restart_identical_resume test_session_restart_identical_resume.cpp)
target_link_libraries(test_session_restart_identical_resume PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_session_restart_identical_resume COMMAND test_session_restart_identical_resume)

# ---- Milestone 4 Phase H2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Roadmap exit criterion (019 §7 G3): Suspended holds zero resources -- E2's census proof, now
# exercised end-to-end through a real checkpoint -> Dormant -> destroy -> restore-and-reactivate
# cycle on a real quark::Engine, rather than in isolation.
add_executable(test_suspended_zero_resources_e2e test_suspended_zero_resources_e2e.cpp)
target_link_libraries(test_suspended_zero_resources_e2e PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_suspended_zero_resources_e2e COMMAND test_suspended_zero_resources_e2e)

# ---- Milestone 4 Phase H3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Roadmap exit criterion (029 §9 G1): the default (non-vector) memory retrieval path is
# deterministic and replayable -- byte-identical ContextContribution across repeated on_context()
# calls against a fixed memory-worktree tree digest and a fixed turn, with no summarizer/network
# call ever occurring during retrieval.
if(WIN32)
  add_executable(test_memory_retrieval_determinism test_memory_retrieval_determinism.cpp)
  target_link_libraries(test_memory_retrieval_determinism PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_retrieval_determinism COMMAND test_memory_retrieval_determinism)
endif()

# ---- CapabilitySet enforcement mechanism (decisions/ADR-009-capability-set-enforcement-mechanism.md) --
# Header-only (trust/capability.hpp), core-tier -- links only agentengine::core.
add_executable(test_capability_enforcement test_capability_enforcement.cpp)
target_link_libraries(test_capability_enforcement PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_capability_enforcement COMMAND test_capability_enforcement)

# ---- Compile-time capability declaration tags (M2 Phase A task A2) -----------------------------
add_executable(test_capability_declaration_tags test_capability_declaration_tags.cpp)
target_link_libraries(test_capability_declaration_tags PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_capability_declaration_tags COMMAND test_capability_declaration_tags)

# ---- agent.spawn depth/budget bound (decisions/ADR-006-agent-spawn-depth-budget-bound.md) -------
# Header-only (trust/spawn_budget.hpp), core-tier (std only, no crypto -- in-process, never crosses
# a process boundary the way trust/capability_token.hpp's ADR-005 token does) -- links only
# agentengine::core.
add_executable(test_spawn_budget test_spawn_budget.cpp)
target_link_libraries(test_spawn_budget PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_spawn_budget COMMAND test_spawn_budget)

# ---- CodeAct discoverability manifest (resolves OQ-16, OpenQuestions.md) -----------------------
# Header-only (trust/agent_library_manifest.hpp), core-tier -- links only agentengine::core.
add_executable(test_agent_library_manifest test_agent_library_manifest.cpp)
target_link_libraries(test_agent_library_manifest PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_library_manifest COMMAND test_agent_library_manifest)

# ---- Reference-agent prompt assembly (M3 Phase H1, 026-Agent-Facing-Runtime-Surface.md §7/§8) --
# Header-only (core/reference_agent_prompt.hpp, core/token_estimate.hpp), core-tier, model-independent.
add_executable(test_reference_agent_prompt test_reference_agent_prompt.cpp)
target_link_libraries(test_reference_agent_prompt PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_reference_agent_prompt COMMAND test_reference_agent_prompt)

# ---- Egress proxy (M2 Phase F task F1, decisions/ADR-011-first-party-egress-proxy.md) ----------
# Header-only claims (is_blocked_address, reject_crlf) plus real loopback-socket claims via a
# TestHttpServer double, agentengine::net_egress_proxy (src/sandbox/net_egress_proxy.cpp).
add_executable(test_net_egress_proxy test_net_egress_proxy.cpp)
target_link_libraries(test_net_egress_proxy PRIVATE agentengine::net_egress_proxy agentengine_warnings)
add_test(NAME test_net_egress_proxy COMMAND test_net_egress_proxy)

# ---- HTTPS egress TLS client proof (decisions/ADR-013-https-egress-tls-client.md) --------------
# Only built when AGENTENGINE_WITH_HTTPS is explicitly turned ON (root CMakeLists.txt) -- links
# directly against mbedTLS (test-only in-memory certificate generation, a loopback TLS test server)
# in addition to agentengine::tls_client itself.
if(AGENTENGINE_WITH_HTTPS)
  add_executable(test_https_egress test_https_egress.cpp)
  target_link_libraries(test_https_egress PRIVATE
    agentengine::tls_client agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_https_egress COMMAND test_https_egress)

  # ---- Milestone 5 Phase C: host-initiated provider HTTP client -------------------------------
  add_executable(test_provider_http_client test_provider_http_client.cpp)
  target_link_libraries(test_provider_http_client PRIVATE
    agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_provider_http_client COMMAND test_provider_http_client)

  # ---- Milestone 5 Phase D: OpenAI-compatible ChatClient backend (protocol/openai/chat_client.hpp) --
  # Translation logic (D1 request/response, D3 tool-schema shaping, D4 structured-output shaping, D2
  # SSE/chunked-transfer parsing) proven offline against literal wire-format JSON -- no TLS/mbedTLS
  # dependency needed for this target, only the header + core.
  add_executable(test_openai_chat_client_translation test_openai_chat_client_translation.cpp)
  target_link_libraries(test_openai_chat_client_translation PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings)
  add_test(NAME test_openai_chat_client_translation COMMAND test_openai_chat_client_translation)

  # End-to-end proof against a real local TLS server: secret resolution -> request build -> real
  # HTTPS exchange -> response/stream parse, for both chat() and chat_stream().
  add_executable(test_openai_chat_client_live test_openai_chat_client_live.cpp)
  target_link_libraries(test_openai_chat_client_live PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_openai_chat_client_live COMMAND test_openai_chat_client_live)

  # ---- Milestone 5 Phase E: Anthropic ChatClient backend (protocol/anthropic/chat_client.hpp) ------
  add_executable(test_anthropic_chat_client_translation test_anthropic_chat_client_translation.cpp)
  target_link_libraries(test_anthropic_chat_client_translation PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings)
  add_test(NAME test_anthropic_chat_client_translation COMMAND test_anthropic_chat_client_translation)

  add_executable(test_anthropic_chat_client_live test_anthropic_chat_client_live.cpp)
  target_link_libraries(test_anthropic_chat_client_live PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_anthropic_chat_client_live COMMAND test_anthropic_chat_client_live)

  # ---- Live end-to-end against REAL inference servers (label: live-network) ----------------------
  # The two *_live tests above are end-to-end over a real TLS socket but against a CANNED loopback
  # server -- every byte the client receives was written by the test itself, so they prove plumbing
  # and parsing but cannot prove that what this project SENDS is well-formed enough for a real
  # provider to accept (a canned server drains and replies unconditionally; a real one returns HTTP
  # 400). These two close that gap.
  #
  # Both are gated on environment variables and SKIP (exit 0) when unset, so a default `ctest` run on
  # a machine with no credential and no network stays green. They are also LABELLED `live-network`,
  # so a run that means to exclude them entirely can:
  #     ctest -LE live-network
  # and a run that means to exercise only them can:
  #     ctest -L live-network
  # tools/run-live-provider-tests.ps1 populates the environment and does the latter.
  #
  # They are nondeterministic by nature (a live model), which is why nothing in either asserts on
  # model CONTENT -- only on structure. Deterministic replay of a real provider's bytes remains
  # Recording/ReplayChatClient's job (004 §5), not theirs.
  add_executable(test_openrouter_live_e2e test_openrouter_live_e2e.cpp)
  target_link_libraries(test_openrouter_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_openrouter_live_e2e COMMAND test_openrouter_live_e2e)
  set_tests_properties(test_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  add_executable(test_llamacpp_live_e2e test_llamacpp_live_e2e.cpp)
  target_link_libraries(test_llamacpp_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_llamacpp_live_e2e COMMAND test_llamacpp_live_e2e)
  set_tests_properties(test_llamacpp_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- ADR-016 gates G1-G4: host-initiated provider egress address policy + transport --------------
  # The positive control for ADR-016. It relaxes the SSRF blocked-range table on the provider path
  # while leaving it enforced on the WASM guest path -- only defensible if BOTH halves are shown
  # against the SAME live loopback address in the SAME run, which is what this does. Deterministic and
  # offline (a plain-HTTP loopback server, no credential, no egress), so it is a DEFAULT-suite test,
  # deliberately not labelled live-network. ADR-016's G5 (the motivating case really works) is
  # test_llamacpp_live_e2e.cpp's.
  add_executable(test_provider_egress_address_policy test_provider_egress_address_policy.cpp)
  target_link_libraries(test_provider_egress_address_policy PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_provider_egress_address_policy COMMAND test_provider_egress_address_policy)

  # ---- ADR-019: genuinely incremental streaming (Phase C/D2's last self-contained residual) -------
  # Asserts time-to-first-item against a drip server, which is the one currency that distinguishes
  # incremental delivery from replay-after-buffering, plus a byte-by-byte feed of the framing
  # decoders (their whole risk is state across call boundaries). Deterministic, default suite.
  add_executable(test_chat_client_stream_incremental test_chat_client_stream_incremental.cpp)
  target_link_libraries(test_chat_client_stream_incremental PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_chat_client_stream_incremental COMMAND test_chat_client_stream_incremental)
  set_tests_properties(test_chat_client_stream_incremental PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase A: 014 §1/§2, the graph as data and its SHARED validator ----------------
  # Every validator rule is exercised in both directions. A suite that only built correct graphs and
  # watched them pass would be satisfied by a validator that returned success unconditionally, so
  # each rule has a graph that trips it AND the nearest graph that does not. Includes the two cases
  # most easily got wrong: a cyclic graph must VALIDATE (014 §9 Q2 allows cycles; the reflection and
  # group-chat patterns need them), and the reachability walk must terminate on a self-loop rather
  # than hang. Pure data, no actors, no engine -- default suite.
  add_executable(test_workflow_graph_validation test_workflow_graph_validation.cpp)
  target_link_libraries(test_workflow_graph_validation PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_graph_validation COMMAND test_workflow_graph_validation)

  # ---- Milestone 6 Phase B: 014 §2's superstep model, on a REAL quark::Engine --------------------
  # Not TestKit: it has no async carrier, and a supervisor whose whole job is co_await-ing
  # cross-actor asks cannot run under it (breakdown decision 3). The two claims worth a real Engine
  # are measured, not inferred from the code's shape: the superstep barrier is checked on entry/exit
  # TIMESTAMPS (correct output alone would pass against a serialized OR an overlapping
  # implementation), and fan-out overlap is checked against the serial sum (the check that fails if
  # someone writes the obvious co_await-per-node dispatch loop, which is correct and silently turns
  # 014 §3's Concurrent pattern into Sequential). 4 workers / 4 shards per CLAUDE.md.
  add_executable(test_workflow_superstep test_workflow_superstep.cpp)
  target_link_libraries(test_workflow_superstep PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_superstep COMMAND test_workflow_superstep)
  set_tests_properties(test_workflow_superstep PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase C: 014 §3's eight patterns, as graph configurations ---------------------
  # §3 claims its patterns are "configurations of the graph, not separate subsystems". Every row is
  # built here from nothing but §1 vocabulary and run on the Phase B engine, so what is ABSENT is
  # part of the proof. Carries the I3 boundary check for routing: an executor -- in production, one
  # fed by a model -- can select among declared edges and nothing more; an invented label must reach
  # nothing AND be reported, never silently end a branch with an empty result.
  add_executable(test_workflow_patterns test_workflow_patterns.cpp)
  target_link_libraries(test_workflow_patterns PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_patterns COMMAND test_workflow_patterns)
  set_tests_properties(test_workflow_patterns PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase D: 014 §6's failure handling ------------------------------------------
  # Covers §6's three bullets and the one thing it leaves open. The phase turns on there being TWO
  # failure channels: a body that RETURNS an error (the actor is healthy; the edge's declared policy
  # decides) and a body that THROWS (Quark 007 restarts the actor and dead-letters the pending ask).
  # A suite that only exercised `ok = false` would prove half of §6 and would never discover whether
  # a faulting executor stalls the supervisor -- so D5 throws, and asserts on a COMPLETED run rather
  # than merely on a non-crash. The classification checks are the other load-bearing pair: a
  # `contract` failure is invoked ONCE under a budget of 3 (deterministic, so retrying learns
  # nothing) and a `policy` failure once under a budget of 5 (a denial re-asked is an I2 concern),
  # with D2's retried `transient` failure as their positive control.
  add_executable(test_workflow_failure test_workflow_failure.cpp)
  target_link_libraries(test_workflow_failure PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_failure COMMAND test_workflow_failure)
  set_tests_properties(test_workflow_failure PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase E: 014 §4's request port, and the activation half of §8 G5 --------------
  # The load-bearing check is E4's CENSUS. `ActorRef::passivate()` drains in-flight work and is never
  # a mid-handler interrupt, so the tempting implementation of §4 -- co_await the human's response
  # inside the running handler -- would keep the activation alive and G5 would be false while every
  # other assertion here still passed. Only `went_dormant()` distinguishes the two designs. E3 is
  # OQ-4's own case (several ports open on one run), which no test in this codebase had ever
  # exercised: every prior use of `Interaction` opens at most one.
  add_executable(test_workflow_request_port test_workflow_request_port.cpp)
  target_link_libraries(test_workflow_request_port PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_request_port COMMAND test_workflow_request_port)
  set_tests_properties(test_workflow_request_port PROPERTIES TIMEOUT 120)

  # ---- Independent QA pass on Milestone 6 Phase E (014 §4's request port) ------------------------
  # A SECOND, independent test file over the same request-port surface, derived from the RFC 014 §4
  # text and the public struct/enum declarations rather than from reading test_workflow_request_port's
  # own author's implementation-derived assertions. Probes: deadline accounting across a suspension,
  # a port inside a cycle opening twice with different derived ids, §6 partial-result preservation
  # against a routing failure discovered at resume time, a port and a failing sibling reached in the
  # same fan-out round, `routes` on a non-switch_case port edge (I3), and open_interactions() parity
  # in the OQ-4 two-port case including a rejected double-resolve.
  add_executable(test_workflow_request_port_independent_qa test_workflow_request_port_independent_qa.cpp)
  target_link_libraries(test_workflow_request_port_independent_qa PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_request_port_independent_qa COMMAND test_workflow_request_port_independent_qa)
  set_tests_properties(test_workflow_request_port_independent_qa PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase F: 014 §5's checkpoint/resume (checkpoint.hpp + WorkflowSupervisor::to_record) --
  # F1: checkpoint.hpp's own two-phase pending->committed discipline against a bare InMemoryStore --
  # decision 7's own claim, that a crash between the two phases falls back to the prior FULLY
  # committed checkpoint, not the dangling one. F2: to_record()/restore_from_record() against a real
  # suspended run, reloaded onto a genuinely NEW actor instance and driven to completion two ways
  # (ContinueWorkflow from a pre-suspension checkpoint, a real ResumeWorkflow from the
  # post-suspension one -- Phase E's own G5 second half, "resumes after a process restart", which
  # had nothing to restore FROM before this file).
  add_executable(test_workflow_checkpoint test_workflow_checkpoint.cpp)
  target_link_libraries(test_workflow_checkpoint PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_checkpoint COMMAND test_workflow_checkpoint)
  set_tests_properties(test_workflow_checkpoint PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase F: 014 §5's time-travel (time_travel.hpp) -------------------------------
  # rewind_workflow() selects a retained checkpoint and durably audits the request (decision 9: the
  # audit obligation built WITH the rewind) -- a separate EventLog/ActorId from the checkpoint log
  # itself (mixing two Event types in one Quark event log would break replay's type-key check).
  add_executable(test_workflow_time_travel test_workflow_time_travel.cpp)
  target_link_libraries(test_workflow_time_travel PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_time_travel COMMAND test_workflow_time_travel)
  set_tests_properties(test_workflow_time_travel PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase F: 014 §8's own promotion gate G2 ---------------------------------------
  # A 20-node chain, killed (simulated: a genuinely new instance restored from nothing but the
  # checkpoint) at EVERY one of its 20 superstep boundaries -- every resume must complete with
  # output identical to an uninterrupted control. 21 short-lived engines in sequence, never
  # concurrent -- a longer timeout than this file's siblings to give that room.
  add_executable(test_workflow_checkpoint_g2 test_workflow_checkpoint_g2.cpp)
  target_link_libraries(test_workflow_checkpoint_g2 PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_checkpoint_g2 COMMAND test_workflow_checkpoint_g2)
  set_tests_properties(test_workflow_checkpoint_g2 PROPERTIES TIMEOUT 300)

  # ---- Milestone 6 Phase G: 014 §7's rendering and diffing (introspection.hpp) -------------------
  # Render is a TOTAL function -- the falsifiable bar is "a valid workflow that cannot be drawn" --
  # proven against a graph exercising all four executor kinds, all six edge kinds, a fallback
  # branch, and author ids containing characters (quotes) that would break naive Mermaid/DOT syntax
  # if node identifiers were sanitized author text instead of index-derived tokens. Diff is a plain
  # structural comparison: add/remove/change an executor, add/remove an edge, start/output/bound
  # changes, and an unchanged graph diffing to empty.
  add_executable(test_workflow_introspection test_workflow_introspection.cpp)
  target_link_libraries(test_workflow_introspection PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_introspection COMMAND test_workflow_introspection)
  set_tests_properties(test_workflow_introspection PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase G: 014 §7's live-view bullet (WorkflowSupervisor::enable_live_view) -----
  # One WorkflowLiveEvent per superstep boundary, over a real ae::stream<T> -- the same graph
  # test_workflow_checkpoint.cpp's own F2 block uses, checked from the executor-state angle instead
  # of the checkpoint angle, plus the negative control (a round that fails the whole run produces
  # no live event, proven by a real wait, not just an unchecked assumption).
  add_executable(test_workflow_live_view test_workflow_live_view.cpp)
  target_link_libraries(test_workflow_live_view PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_live_view COMMAND test_workflow_live_view)
  set_tests_properties(test_workflow_live_view PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase H: 030's Project manifest and registry (project.hpp, registry.hpp) ------
  # H1-H3: the registry actor (create/duplicate-reject/principal-scoped list/recovery). H4-H5: the
  # manifest's Snapshot round-trip and the archived tail's EventLog round-trip. G4 (030 §7's own
  # gate): a manifest's serialized write cost is measured to be independent of archived-tail size
  # (growing the tail to 2000 entries leaves the SAME manifest's own tagged_object_size unchanged)
  # and scales only with current active-member count, never with history -- the forced EventLog/
  # Snapshot split project.hpp's own header comment explains is what makes this even possible.
  add_executable(test_project_registry test_project_registry.cpp)
  target_link_libraries(test_project_registry PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_project_registry COMMAND test_project_registry)
  set_tests_properties(test_project_registry PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase I: 030 §4's directed lifecycle (lifecycle.hpp) --------------------------
  # PassivatableHandle's real point, proven with REAL actors: one ProjectSupervisor holds a REAL
  # AgentSession<CannedChatClient> (a member session) and a REAL WorkflowSupervisor (a
  # workflow-supervising actor, 030 §8 Q4's own case) -- two different C++ actor types, passivated
  # uniformly. Pause drops all three activations (session, workflow, the ProjectSupervisor itself,
  # last) to Dormant by census; restore never reactivates anything; a real Run after restore
  # continues the session's own run sequence untouched; archive reuses the same pause sequence.
  add_executable(test_project_lifecycle test_project_lifecycle.cpp)
  target_link_libraries(test_project_lifecycle PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_project_lifecycle COMMAND test_project_lifecycle)
  set_tests_properties(test_project_lifecycle PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase J: 014 §8's own promotion gate G1 (half 1 of the milestone exit criterion) --
  # Reuses test_workflow_patterns.cpp's own eight §3 graph constructions, injecting a transient
  # failure (retry-recovered) or a bounded delay (a real, short sleep inside an executor body --
  # test_workflow_superstep.cpp's own B2 precedent) into one participating executor per pattern.
  # Every assertion is the SAME correctness check the happy-path test already made.
  add_executable(test_workflow_patterns_resilience test_workflow_patterns_resilience.cpp)
  target_link_libraries(test_workflow_patterns_resilience PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_patterns_resilience COMMAND test_workflow_patterns_resilience)
  set_tests_properties(test_workflow_patterns_resilience PROPERTIES TIMEOUT 120)

  # ---- Milestone 6 Phase J: 030 §7's own promotion gate G1 (half 2 of the milestone exit criterion) --
  # 100 real AgentSession actors, each with its own ProjectSupervisor (200 activations, an actor
  # count per decision 8's own reasoning, never a thread count). Pausing #50 drops its own two
  # activations to zero by census; the other 99 each complete a real second Run with their own
  # run-id sequence uninterrupted, and a timed sample shows no structural latency regression.
  add_executable(test_project_scale_isolation test_project_scale_isolation.cpp)
  target_link_libraries(test_project_scale_isolation PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_project_scale_isolation COMMAND test_project_scale_isolation)
  set_tests_properties(test_project_scale_isolation PROPERTIES TIMEOUT 180)

  # ---- ADR-020 gates G1-G7: 004 §2's portable reasoning_effort, mapped down by both backends ------
  # Drives BOTH backends from the same enumerator values in one file, on purpose: the amendment's
  # claim is portability across two structurally different native shapes (a flat string vs a computed
  # token budget), and a per-backend suite could only ever prove half of it. Carries the two negative
  # controls the design turns on -- the capability gate refusing a level, and Anthropic's own vendor
  # constraints being enforced client-side because a gateway was measured NOT enforcing them.
  # Pure request building, no network, so it is a default-suite test.
  add_executable(test_reasoning_effort_portability test_reasoning_effort_portability.cpp)
  target_link_libraries(test_reasoning_effort_portability PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_reasoning_effort_portability COMMAND test_reasoning_effort_portability)

  # ---- ADR-018: a REAL ChatClient backend driven through a live AgentSession turn loop -------------
  # Closes Milestone 5's Phase J1 residual. Before it, AgentSession<OpenAIChatClient<...>> did not
  # merely lack a way to be configured -- it failed to COMPILE, because quark::TestKit<A> default-
  # constructs its actor and a real backend holds a `Store const&`. So 001 §3's turn loop and a
  # backend that actually talks to a server had never been demonstrated together. Deterministic
  # (canned loopback server, no live model), so it belongs in the default suite.
  add_executable(test_agent_session_real_backend test_agent_session_real_backend.cpp)
  target_link_libraries(test_agent_session_real_backend PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_agent_session_real_backend COMMAND test_agent_session_real_backend)

  # ---- Milestone 5 Phase J1 (roadmap exit criterion, 004 §7 G1): the same unchanged call site, run
  # against real OpenAI/Anthropic backends over local TLS servers, a ReplayChatClient, and a
  # FailoverChatClient<real, real> composition with a genuinely failing primary.
  add_executable(test_chat_client_cross_backend_parity test_chat_client_cross_backend_parity.cpp)
  target_link_libraries(test_chat_client_cross_backend_parity PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_chat_client_cross_backend_parity COMMAND test_chat_client_cross_backend_parity)

  # ---- Milestone 5 Phase J2 (roadmap exit criterion, 018 §7 G2): plant a canary secret, scan every
  # persisted artifact this milestone's mechanisms actually produce (recordings, checkpoints,
  # error-path text) for zero occurrences, paired with a positive control proving the secret really
  # was exercised as a live credential.
  add_executable(test_secret_hygiene_canary_scan test_secret_hygiene_canary_scan.cpp)
  target_link_libraries(test_secret_hygiene_canary_scan PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_secret_hygiene_canary_scan COMMAND test_secret_hygiene_canary_scan)

  # ---- Milestone 5 Phase J3 (roadmap exit criterion, 004 §7 G2): the common-case (fast provider)
  # bounded-release proof, plus a real, quantified finding that chat_stream()'s own fetch has no
  # stop_token wired to the consumer's cancellation -- named honestly, not silently claimed complete.
  add_executable(test_chat_client_stream_cancellation_bounded test_chat_client_stream_cancellation_bounded.cpp)
  target_link_libraries(test_chat_client_stream_cancellation_bounded PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_chat_client_stream_cancellation_bounded COMMAND test_chat_client_stream_cancellation_bounded)

  # ---- Described<T, "..."> (core/json_schema.hpp): per-parameter tool descriptions, proven to
  # survive both OpenAI's and Anthropic's real translate_tool() end to end.
  add_executable(test_json_schema_described test_json_schema_described.cpp)
  target_link_libraries(test_json_schema_described PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings)
  add_test(NAME test_json_schema_described COMMAND test_json_schema_described)
endif()

# ---- RecordedChatClient fixture player (test-only) ---------------------------------------------
# Proves the ChatClient concept and ChatRequest/ChatResponse vocabulary are usable end-to-end
# against hand-authored JSON fixtures, without a real network call or API key (CLAUDE.md task
# brief; concept: 004-Model-Provider-Plane.md §6). nlohmann/json is a test-only dependency per
# CONVENTIONS.md's dependency-tier discipline: FetchContent-ed here (not in the root CMakeLists.txt)
# and linked only to this test target — never to agentengine_core or anything under
# include/agentengine/core.
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(test_recorded_chat_client test_recorded_chat_client.cpp)
target_link_libraries(test_recorded_chat_client PRIVATE
  agentengine::core agentengine_warnings nlohmann_json::nlohmann_json)
target_compile_definitions(test_recorded_chat_client PRIVATE
  AE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/chat_client")

add_test(NAME test_recorded_chat_client COMMAND test_recorded_chat_client)

# ---- Milestone 5 Phase A (docs/planning/milestone-5-providers-identity-secrets-breakdown.md) ---
# SecretRef/SecretLease/SecretStore (trust/secret.hpp), a capability-gated wrapper over Quark's own
# SecretSource seam (020-Security §4) -- env/file resolution, scoping per name, rotation without
# restart, fail-closed without a granted cap::Secret.
add_executable(test_secret_store test_secret_store.cpp)
target_link_libraries(test_secret_store PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_secret_store COMMAND test_secret_store)

# ---- Milestone 5 Phase B1/B2/B5/B6 -- the ChatClient concept made real -------------------------
# ChatRequest.tools/output_schema_json, the completed ChatClientCapabilities bitset,
# select_output_schema_strategy (004 §2's degradation rule), ChatClientRegistry.
add_executable(test_chat_client_provider_plane test_chat_client_provider_plane.cpp)
target_link_libraries(test_chat_client_provider_plane PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_chat_client_provider_plane COMMAND test_chat_client_provider_plane)

# ---- Milestone 5 Phase B6 -- register_agent<A>()'s additive ChatClientRegistry parameter --------
add_executable(test_agent_registry_chat_client_registry test_agent_registry_chat_client_registry.cpp)
target_link_libraries(test_agent_registry_chat_client_registry PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_registry_chat_client_registry COMMAND test_agent_registry_chat_client_registry)

# ---- Milestone 5 Phase B3 -- outbound-credential resolution at the point of use (004 §1/018 §4) --
add_executable(test_chat_client_credential_resolution test_chat_client_credential_resolution.cpp)
target_link_libraries(test_chat_client_credential_resolution PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_chat_client_credential_resolution COMMAND test_chat_client_credential_resolution)

# ---- Milestone 5 Phase B4b -- ae::stream<T> (core/stream.hpp), the AgentEngine-side adapter over
# Quark's ReplyStream/StreamChannel credit-controlled ring (RFC 024/ADR-018) -- against a real
# ChatClient conformer that streams for real across a background producer thread.
add_executable(test_chat_client_stream test_chat_client_stream.cpp)
target_link_libraries(test_chat_client_stream PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_chat_client_stream COMMAND test_chat_client_stream)

# ---- Milestone 5 Phase F1/F2 -- ResilientChatClient<Inner> (retry + Quark 022 circuit breaking),
# core/resilient_chat_client.hpp, against a hand-rolled scripted ChatClient conformer.
add_executable(test_resilient_chat_client test_resilient_chat_client.cpp)
target_link_libraries(test_resilient_chat_client PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_resilient_chat_client COMMAND test_resilient_chat_client)

# ---- Milestone 5 Phase F3 -- FailoverChatClient<Primary, Fallback...> (004 §4 explicit failover),
# core/failover_chat_client.hpp.
add_executable(test_failover_chat_client test_failover_chat_client.cpp)
target_link_libraries(test_failover_chat_client PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_failover_chat_client COMMAND test_failover_chat_client)

# ---- Milestone 5 Phase F4 -- per-run TokenBudget<N> enforcement at the turn boundary
# (AgentSession::handle, agent_session.hpp).
add_executable(test_agent_session_token_budget test_agent_session_token_budget.cpp)
target_link_libraries(test_agent_session_token_budget PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_token_budget COMMAND test_agent_session_token_budget)

# ---- Milestone 5 Phase G1 -- ChatCallRecording JSON codec (core/chat_recording.hpp), the shared
# foundation RecordingChatClient<Inner> and ReplayChatClient both depend on.
add_executable(test_chat_recording_codec test_chat_recording_codec.cpp)
target_link_libraries(test_chat_recording_codec PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_chat_recording_codec COMMAND test_chat_recording_codec)

# ---- Milestone 5 Phase G1 -- RecordingChatClient<Inner> (core/recording_chat_client.hpp), the real
# recording mechanism promoted from RecordedChatClient's test-scoped stand-in.
add_executable(test_recording_chat_client test_recording_chat_client.cpp)
target_link_libraries(test_recording_chat_client PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_recording_chat_client COMMAND test_recording_chat_client)

# ---- Milestone 5 Phase G2 -- ReplayChatClient (core/replay_chat_client.hpp), the real replay
# mechanism with identical chunk boundaries and inter-chunk timing (004 §7 G3).
add_executable(test_replay_chat_client test_replay_chat_client.cpp)
target_link_libraries(test_replay_chat_client PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_replay_chat_client COMMAND test_replay_chat_client)

# ---- Milestone 5 Phase H1 -- Principal establishment/delegation/admission mechanism
# (trust/principal.hpp): make_embedded_principal/make_local_cli_principal/make_anonymous_principal,
# derive_on_behalf_of (007 §2, depth-bounded), principal_admitted_for (018 §2's shared ownership rule).
add_executable(test_principal_delegation test_principal_delegation.cpp)
target_link_libraries(test_principal_delegation PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_principal_delegation COMMAND test_principal_delegation)

# ---- Milestone 5 Phase H2 -- admission check at the AgentSession actor boundary (018 §2),
# StartRun::caller (agent_session.hpp), opt-in and additive over the pre-H2 unconditional-admission
# behavior every pre-existing StartRun{message} call site across tests/ still relies on.
add_executable(test_agent_session_admission test_agent_session_admission.cpp)
target_link_libraries(test_agent_session_admission PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_admission COMMAND test_agent_session_admission)

# ---- Milestone 7 Phase A -- 013 §1's internal run event stream (run_event.hpp,
# AgentSession::enable_event_stream()): real emission at the turn loop's actual boundaries -----------
add_executable(test_agent_session_run_event_stream test_agent_session_run_event_stream.cpp)
target_link_libraries(test_agent_session_run_event_stream PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_run_event_stream COMMAND test_agent_session_run_event_stream)

# ---- Milestone 7 Phase B -- 006 §6b's Backgroundable/StandingEffect (standing_effect.hpp,
# tool_pipeline.hpp::background_task(), AgentSession::start_background_task()) -----------------------
add_executable(test_agent_session_background_task test_agent_session_background_task.cpp)
target_link_libraries(test_agent_session_background_task PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_background_task COMMAND test_agent_session_background_task)

# ---- Milestone 7 Phase C1 -- 011 MCP's JSON-RPC 2.0 wire envelope (protocol/mcp/json_rpc.hpp) ------
add_executable(test_mcp_json_rpc test_mcp_json_rpc.cpp)
target_link_libraries(test_mcp_json_rpc PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_json_rpc COMMAND test_mcp_json_rpc)

# ---- Milestone 7 Phase C2 -- 011 §4's MCP server role (protocol/mcp/server.hpp): server/discover,
# tools/list, tools/call over a real ToolTable and the real invoke_tool() pipeline -------------------
add_executable(test_mcp_server test_mcp_server.cpp)
target_link_libraries(test_mcp_server PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_server COMMAND test_mcp_server)

# ---- Milestone 7 Phase C3 -- 011 §3's MCP client role (protocol/mcp/client.hpp): caching,
# pagination, digest-pinning/rug-pull detection, isError surfaced not thrown -------------------------
add_executable(test_mcp_client test_mcp_client.cpp)
target_link_libraries(test_mcp_client PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_client COMMAND test_mcp_client)

# ---- Milestone 7 Phase C4 -- the io.modelcontextprotocol/tasks extension (011 §3.6/§12): a
# Backgroundable tool call started via a real McpServer, polled to completion via tasks/get, cancelled
# via tasks/cancel -- reusing background_task() (real since Phase B) end to end -------------------
add_executable(test_mcp_tasks_extension test_mcp_tasks_extension.cpp)
target_link_libraries(test_mcp_tasks_extension PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_tasks_extension COMMAND test_mcp_tasks_extension)

# ---- Milestone 7 Phase D1 -- 012 §0/§A.4's A2A v1.0 wire object model (protocol\a2a\types.hpp):
# Part/Message/Artifact/TaskStatus/Task round-trip, the removed `kind` discriminator, full
# SCREAMING_SNAKE enum serialization, and unknown-Part-kind forward compatibility -----------------
add_executable(test_a2a_types test_a2a_types.cpp)
target_link_libraries(test_a2a_types PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_a2a_types COMMAND test_a2a_types)

# ---- Milestone 5 Phase H4 -- EffectContext::principal wired through every outbound ChatClient
# call, including on_behalf_of delegation for a session initialized with a derive_on_behalf_of()'d
# principal (018 §1's "delegation only via on_behalf_of, never token passthrough").
add_executable(test_agent_session_delegation test_agent_session_delegation.cpp)
target_link_libraries(test_agent_session_delegation PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_delegation COMMAND test_agent_session_delegation)

# ---- Dependency-free JSON value/parser/serializer (M2 Phase B task B2, core/json_value.hpp) ----
# Header-only, core-tier -- links only agentengine::core (no nlohmann::json; this IS the seam that
# keeps nlohmann out of product code, see CONVENTIONS.md dependency-tier discipline).
add_executable(test_json_value test_json_value.cpp)
target_link_libraries(test_json_value PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_json_value COMMAND test_json_value)

# ---- wasmtime C API smoke proof (M2 Phase D task D1, decision 5) -------------------------------
# Only built when AGENTENGINE_WITH_WASM is explicitly turned ON (root CMakeLists.txt) -- proves the
# vendored dependency itself links and runs a real WebAssembly module; no SandboxBackend exists yet
# (see src/backends/wasm/README.md and this test file's own header).
if(AGENTENGINE_WITH_WASM)
  add_executable(test_wasmtime_smoke test_wasmtime_smoke.cpp)
  target_link_libraries(test_wasmtime_smoke PRIVATE agentengine::wasmtime_vendor)
  add_test(NAME test_wasmtime_smoke COMMAND test_wasmtime_smoke)
  if(WIN32)
    # Windows has no rpath; wasmtime.dll must be reachable via PATH at run time.
    set_tests_properties(test_wasmtime_smoke PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")
  endif()
  # Linux links libwasmtime.a (static) -- see root CMakeLists.txt's AGENTENGINE_WITH_WASM block for
  # why -- so no run-time library path wiring is needed there.

  # ---- wasm SandboxBackend proof (M2 Phase D task D3, decisions/ADR-010-*.md) -------------------
  # Builds tests/fixtures/wasm_ae_tool_fixture/'s real ae:tool component via cargo-component when
  # that toolchain is present (checked with find_program, never assumed) -- the compiled .wasm is
  # NOT committed to the repo (.gitignore's own "WASM plugin build output" rule, project-wide, not
  # a rule this task carves an exception into). When the toolchain is absent, the test itself
  # SKIPs (SKIP_RETURN_CODE 77, the same mechanism test_shell_runner_no_process_creation already
  # uses for "tool not found on this machine") rather than failing or silently reporting a false
  # pass.
  find_program(AE_CARGO_EXE cargo)
  find_program(AE_CARGO_COMPONENT_EXE cargo-component)
  set(AE_WASM_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wasm_ae_tool_fixture")
  set(AE_WASM_FIXTURE_WASM
    "${AE_WASM_FIXTURE_DIR}/target/wasm32-unknown-unknown/release/ae_tool_fixture.wasm")
  if(AE_CARGO_EXE AND AE_CARGO_COMPONENT_EXE)
    add_custom_command(
      OUTPUT "${AE_WASM_FIXTURE_WASM}"
      COMMAND "${AE_CARGO_EXE}" component build --release --target wasm32-unknown-unknown
      WORKING_DIRECTORY "${AE_WASM_FIXTURE_DIR}"
      DEPENDS
        "${AE_WASM_FIXTURE_DIR}/src/lib.rs"
        "${AE_WASM_FIXTURE_DIR}/Cargo.toml"
        "${AE_WASM_FIXTURE_DIR}/wit/ae-tool.wit"
      COMMENT "Building D3 test fixture (cargo-component): ae_tool_fixture.wasm")
    add_custom_target(ae_wasm_fixture ALL DEPENDS "${AE_WASM_FIXTURE_WASM}")
  endif()

  add_executable(test_wasm_backend test_wasm_backend.cpp)
  target_link_libraries(test_wasm_backend PRIVATE agentengine::wasm_backend)
  target_compile_definitions(test_wasm_backend PRIVATE
    AE_WASM_FIXTURE_PATH="${AE_WASM_FIXTURE_WASM}")
  if(TARGET ae_wasm_fixture)
    add_dependencies(test_wasm_backend ae_wasm_fixture)
  endif()
  add_test(NAME test_wasm_backend COMMAND test_wasm_backend)
  set_tests_properties(test_wasm_backend PROPERTIES SKIP_RETURN_CODE 77)
  if(WIN32)
    set_tests_properties(test_wasm_backend PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")
  endif()
endif()

# ---- Tool JSON-Schema derivation (M2 Phase B task B1, 006 §1) ----------------------------------
# Header-only (core/json_schema.hpp, core/tool.hpp); nlohmann_json is test-only here too (parses
# the emitted schema string to assert real JSON structure, not substring-match it).
add_executable(test_tool_json_schema test_tool_json_schema.cpp)
target_link_libraries(test_tool_json_schema PRIVATE
  agentengine::core agentengine_warnings nlohmann_json::nlohmann_json)
add_test(NAME test_tool_json_schema COMMAND test_tool_json_schema)

# ---- Tool JSON codec: to_json/from_json (M2 Phase B task B2, 006 §3 steps 2/9) ------------------
# Header-only (core/json_schema.hpp's AE_JSON_SCHEMA-generated ae_to_json/ae_from_json), core-tier
# -- links only agentengine::core (no nlohmann::json; exercises the dependency-free codec itself).
add_executable(test_json_schema_codec test_json_schema_codec.cpp)
target_link_libraries(test_json_schema_codec PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_json_schema_codec COMMAND test_json_schema_codec)

# ---- Ten-step tool invocation pipeline (M2 Phase B tasks B2/B3, 006 §3, miniature §8 G2) --------
# Header-only (core/tool_pipeline.hpp), core-tier -- links only agentengine::core.
add_executable(test_tool_pipeline test_tool_pipeline.cpp)
target_link_libraries(test_tool_pipeline PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_tool_pipeline COMMAND test_tool_pipeline)

# ---- Capability-handle-reuse-denial, at the pipeline level (M2 Phase B task B4, 006 §8 G3) ------
add_executable(test_tool_pipeline_capability_reuse test_tool_pipeline_capability_reuse.cpp)
target_link_libraries(test_tool_pipeline_capability_reuse PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_tool_pipeline_capability_reuse COMMAND test_tool_pipeline_capability_reuse)

# ---- call_tool bridge: sandbox-scoped invoke_tool(), bundled approval, tainted results (Milestone 3
# Phase F2, 010 §6 / 006 §3). Header-only (src/backends/native_jail/tool_bridge.hpp), core-tier --
# links only agentengine::core, no native_jail runner target needed for this proof.
add_executable(test_tool_bridge test_tool_bridge.cpp)
target_include_directories(test_tool_bridge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_tool_bridge PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_tool_bridge COMMAND test_tool_bridge)

# ---- Output discipline: cap_output()'s truncation/marker/UTF-8-boundary math (Milestone 3 Phase F3,
# 010 §3 items 4/5). Header-only (src/backends/native_jail/output_discipline.hpp), no Python/sandbox
# dependency -- Runner-level result_repr capture and integration is covered in the Python-gated
# MediatedPythonRunner smoke test instead.
add_executable(test_output_discipline test_output_discipline.cpp)
target_include_directories(test_output_discipline PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_output_discipline PRIVATE agentengine_warnings)
add_test(NAME test_output_discipline COMMAND test_output_discipline)

# ---- agent.tools codegen: schema -> Python-source generation (Milestone 3 Phase G1, 026 §4/§5).
# Header-only (src/backends/native_jail/agent_tools_codegen.hpp), core-tier -- links only
# agentengine::core for ToolDescriptor/json::Value, no Python.h dependency at all. The generated
# source actually RUNNING is proven separately, Python-gated, below.
add_executable(test_agent_tools_codegen test_agent_tools_codegen.cpp)
target_include_directories(test_agent_tools_codegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_agent_tools_codegen PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_tools_codegen COMMAND test_agent_tools_codegen)

# ---- agent.files/agent.data codegen: fixed static Python source (Milestone 3 Phase G2, 026 §5).
# Header-only (src/backends/native_jail/agent_files_data_codegen.hpp), core-tier -- no Python.h
# dependency. The generated source actually RUNNING is proven separately, Python-gated, below.
add_executable(test_agent_files_data_codegen test_agent_files_data_codegen.cpp)
target_include_directories(test_agent_files_data_codegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_agent_files_data_codegen PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_files_data_codegen COMMAND test_agent_files_data_codegen)

# ---- Agent metadata compiler: register_agent<A>() (M2 Phase E task E2, 002 §6) -----------------
# Header-only (core/agent_registry.hpp), core-tier -- links only agentengine::core.
add_executable(test_agent_registry test_agent_registry.cpp)
target_link_libraries(test_agent_registry PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_registry COMMAND test_agent_registry)

# ---- SandboxProfile<P>'s template-parameter kind (decisions/ADR-012-sandbox-profile-template-parameter-kind.md) --
# Header-only (core/agent_registry.hpp, sandbox/sandbox.hpp), core-tier.
add_executable(test_sandbox_profile_kind test_sandbox_profile_kind.cpp)
target_link_libraries(test_sandbox_profile_kind PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_sandbox_profile_kind COMMAND test_sandbox_profile_kind)

# ---- Agent tool invocation end-to-end (M2 Phase E task E3, the milestone's headline sentence) ---
# Header-only (core\agent_registry.hpp's invoke_agent_tool(), core\tool_pipeline.hpp), core-tier.
add_executable(test_agent_tool_invocation test_agent_tool_invocation.cpp)
target_link_libraries(test_agent_tool_invocation PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_tool_invocation COMMAND test_agent_tool_invocation)

# ---- Policy-reachability enumerator (M2 Phase F task F2, 007 §9 gate G6) -----------------------
# Proves include/agentengine/trust/policy_reachability.hpp's enumerator against
# ../tools/policy_reachability_fixture.hpp's reference set -- the same fixture
# tools/policy_reachability.cpp (the CI tool itself) runs, so the two can never drift apart.
# Header-only, core-tier.
add_executable(test_policy_reachability test_policy_reachability.cpp)
target_include_directories(test_policy_reachability PRIVATE "${CMAKE_SOURCE_DIR}/tools")
target_link_libraries(test_policy_reachability PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_policy_reachability COMMAND test_policy_reachability)

# ---- SandboxBackend contract completion (M2 Phase C task C1, 008 §2-3) -------------------------
# ProfileTraits / resolve_strict (Profile::Strict resolution) and MountSpec's host-path/blob-store
# source field. Header-only, core-tier -- links only agentengine::core.
add_executable(test_sandbox_backend_contract test_sandbox_backend_contract.cpp)
target_link_libraries(test_sandbox_backend_contract PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_sandbox_backend_contract COMMAND test_sandbox_backend_contract)

# ---- ExecState / SessionExecStateRegistry (010 §3a, Milestone 3 Phase E1) -----------------------
# Header-only (sandbox/runner.hpp) -- no platform-specific dependency, proven with mock Runners
# since PythonRunner/ShellRunner (Phase E2/E3) don't exist yet.
add_executable(test_exec_state test_exec_state.cpp)
target_link_libraries(test_exec_state PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_exec_state COMMAND test_exec_state)

# ---- native_jail Runner stub contract ---------------------------------------------------------
# Proves PythonRunner (still a stub) satisfies Runner and fails closed, and that ShellRunner (now
# real, see below) satisfies Runner — see the file's own top comment for exactly what is and is
# not covered.
add_executable(test_native_jail_runner_stubs test_native_jail_runner_stubs.cpp)
target_link_libraries(test_native_jail_runner_stubs PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)

add_test(NAME test_native_jail_runner_stubs COMMAND test_native_jail_runner_stubs)

# ---- ShellRunner prove-phase gates (decisions/ADR-001-shellrunner-grammar-and-dispatch.md) -----

# RealFileSystemAdapter's path-escape list (§2.5.5).
add_executable(test_real_filesystem_adapter test_real_filesystem_adapter.cpp)
target_link_libraries(test_real_filesystem_adapter PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)
add_test(NAME test_real_filesystem_adapter COMMAND test_real_filesystem_adapter)

# End-to-end ShellRunner claims: Sh-C1, Sh-C2, Sh-C3, Sh-G4, kind-only capability checks, A-C2,
# Sh-S1's behavioral half.
add_executable(test_shell_runner_proof test_shell_runner_proof.cpp)
target_link_libraries(test_shell_runner_proof PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)
add_test(NAME test_shell_runner_proof COMMAND test_shell_runner_proof)

# A-S1: parser safety against a hand-written adversarial corpus. Fake-free — links only against
# the parser, not the dispatch/fs layer, matching the ADR's own "parse() needs nothing but a byte
# string" claim.
add_executable(test_shell_parser_adversarial test_shell_parser_adversarial.cpp)
target_link_libraries(test_shell_parser_adversarial PRIVATE
  agentengine::core agentengine::shell_runner agentengine_warnings)
add_test(NAME test_shell_parser_adversarial COMMAND test_shell_parser_adversarial)
# The literal ADR-named "100k-stage pipeline" figure is only compiled into a Release build (see
# test_shell_parser_adversarial.cpp's comment on this): the default configuration here links the
# MSVC debug CRT, under which that width triggers a checked-iterator/debug-STL slowdown severe
# enough to hang a routine `ctest` run (CLAUDE.md Machine Safety) — a real build-configuration
# finding from this pass, not a parser defect (confirmed via a side-by-side Release measurement).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_definitions(test_shell_parser_adversarial PRIVATE AE_RUN_100K_PIPELINE_PROBE)
endif()
# This test allocates ~12 MiB arenas repeatedly and runs bounded (kMaxTokens/kMaxNestingDepth-
# sized) adversarial loops — CLAUDE.md machine-safety timeout as a backstop, not because it is
# expected to run anywhere near this long.
set_tests_properties(test_shell_parser_adversarial PROPERTIES TIMEOUT 60)

# Sh-S1's static half: no process-creation symbol is referenced anywhere in the
# agentengine_shell_runner library's compiled object code (ADR-001 §7 finding 1's link-target-
# granularity fix). Checked via llvm-nm against the actual built artifact; skips (CTest
# SKIP_RETURN_CODE) if llvm-nm isn't found rather than silently passing.
find_program(AE_LLVM_NM_EXECUTABLE NAMES llvm-nm llvm-nm.exe)
add_executable(test_shell_runner_no_process_creation test_shell_runner_no_process_creation.cpp)
target_link_libraries(test_shell_runner_no_process_creation PRIVATE
  agentengine::core agentengine_warnings)
target_compile_definitions(test_shell_runner_no_process_creation PRIVATE
  AE_SHELL_RUNNER_LIB_PATH="$<TARGET_FILE:agentengine_shell_runner>"
  AE_LLVM_NM_PATH="${AE_LLVM_NM_EXECUTABLE}")
add_test(NAME test_shell_runner_no_process_creation COMMAND test_shell_runner_no_process_creation)
set_tests_properties(test_shell_runner_no_process_creation PROPERTIES SKIP_RETURN_CODE 77)

# ---- Milestone 3 Phase E3's real ShellRunner (mediated_shell_*.{hpp,cpp}) -----------------------
# Windows only (matches agentengine_mediated_shell_runner's own scope, CMakeLists.txt).
if(WIN32)
  add_executable(test_mediated_shell_runner_smoke test_mediated_shell_runner_smoke.cpp)
  target_link_libraries(test_mediated_shell_runner_smoke PRIVATE
    agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_mediated_shell_runner_smoke COMMAND test_mediated_shell_runner_smoke)
  set_tests_properties(test_mediated_shell_runner_smoke PROPERTIES TIMEOUT 60)

  # ---- Milestone 3 Phase E4: hostile-corpus/mediation proof (010 §9 G2/G7) ---------------------
  add_executable(test_mediated_shell_runner_hostile_corpus test_mediated_shell_runner_hostile_corpus.cpp)
  target_link_libraries(test_mediated_shell_runner_hostile_corpus PRIVATE
    agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_mediated_shell_runner_hostile_corpus COMMAND test_mediated_shell_runner_hostile_corpus)
  set_tests_properties(test_mediated_shell_runner_hostile_corpus PROPERTIES TIMEOUT 60)

  # G2's ShellRunner-specific "must not exist as a reachable code path" bar, structural half,
  # against the REAL agentengine_mediated_shell_runner artifact (test_shell_runner_no_process_
  # creation.cpp already covers the untouched spike). Reuses the same llvm-nm discovered above.
  add_executable(test_mediated_shell_runner_no_process_creation
    test_mediated_shell_runner_no_process_creation.cpp)
  target_link_libraries(test_mediated_shell_runner_no_process_creation PRIVATE
    agentengine::core agentengine_warnings)
  target_compile_definitions(test_mediated_shell_runner_no_process_creation PRIVATE
    AE_MEDIATED_SHELL_RUNNER_LIB_PATH="$<TARGET_FILE:agentengine_mediated_shell_runner>"
    AE_LLVM_NM_PATH="${AE_LLVM_NM_EXECUTABLE}")
  add_test(NAME test_mediated_shell_runner_no_process_creation
    COMMAND test_mediated_shell_runner_no_process_creation)
  set_tests_properties(test_mediated_shell_runner_no_process_creation PROPERTIES SKIP_RETURN_CODE 77)

  # RunnerCall<python> composition against the REAL MediatedPythonRunner (E2) -- only built when
  # AGENTENGINE_BUILD_PYTHON_RUNNER is also ON (needs a real CPython install).
  if(AGENTENGINE_BUILD_PYTHON_RUNNER)
    add_executable(test_mediated_shell_runner_python_composition
      test_mediated_shell_runner_python_composition.cpp)
    target_link_libraries(test_mediated_shell_runner_python_composition PRIVATE
      agentengine::core agentengine::mediated_shell_runner agentengine::mediated_python_runner
      agentengine::worktree_store agentengine_warnings)
    target_compile_definitions(test_mediated_shell_runner_python_composition PRIVATE
      AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
    add_test(NAME test_mediated_shell_runner_python_composition
      COMMAND test_mediated_shell_runner_python_composition)
    # AE_PYTHON_TEST_ENV isn't defined yet at this point in the script (set later, alongside the
    # ADR-002/003 prove-phase test block) -- built directly here from AGENTENGINE_PYTHON_DLL_DIR
    # (set by the root CMakeLists.txt before add_subdirectory(tests) runs) for the identical
    # Windows-has-no-rpath reason those tests need it.
    set_tests_properties(test_mediated_shell_runner_python_composition PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}" TIMEOUT 60)
  endif()
endif()

# ---- Job Object resource-limit layer (decisions/ADR-004-appcontainer-native-jail-windows-backend.md) --
# Windows only -- see CMakeLists.txt's agentengine_job_object_limits target comment.
if(WIN32)
  # Deliberately hostile test-only helper process (tests/helpers/hostile_child.cpp) the Job Object
  # tests launch and try to contain -- never a product target, never linked into anything else.
  # ws2_32: C5's probe_net mode uses raw Winsock.
  add_executable(job_object_hostile_child helpers/hostile_child.cpp)
  target_link_libraries(job_object_hostile_child PRIVATE ws2_32)

  add_executable(test_job_object_limits test_job_object_limits.cpp)
  target_link_libraries(test_job_object_limits PRIVATE
    agentengine::core agentengine::job_object_limits agentengine_warnings)
  target_compile_definitions(test_job_object_limits PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_job_object_limits job_object_hostile_child)
  add_test(NAME test_job_object_limits COMMAND test_job_object_limits)
  # Generous but finite: this test spawns real hostile child processes (unbounded memory growth,
  # CPU spin, unbounded process spawning) whose containment is exactly what is under test --
  # CLAUDE.md Machine Safety requires a backstop even though every individual wait inside the test
  # already has its own bounded deadline.
  set_tests_properties(test_job_object_limits PROPERTIES TIMEOUT 90)

  # ---- NativeJailBackend end-to-end (M2 Phase C task C2, 008 §1b/§2/§3) -------------------------
  add_executable(test_native_jail_backend_windows test_native_jail_backend_windows.cpp)
  target_link_libraries(test_native_jail_backend_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(test_native_jail_backend_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_backend_windows job_object_hostile_child)
  add_test(NAME test_native_jail_backend_windows COMMAND test_native_jail_backend_windows)
  # Same Machine Safety backstop as test_job_object_limits -- real child processes, real wall_ms
  # kills under test.
  set_tests_properties(test_native_jail_backend_windows PROPERTIES TIMEOUT 90)

  # ---- 008 §7 abuse-case corpus + §9 G2 positive controls (M2 Phase C task C3) -------------------
  add_executable(test_native_jail_abuse_corpus_windows test_native_jail_abuse_corpus_windows.cpp)
  target_link_libraries(test_native_jail_abuse_corpus_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(test_native_jail_abuse_corpus_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_abuse_corpus_windows job_object_hostile_child)
  add_test(NAME test_native_jail_abuse_corpus_windows COMMAND test_native_jail_abuse_corpus_windows)
  # Five abuse cases, each with a contained run and at least one positive control -- more real
  # child processes than C2's own test, same Machine Safety discipline.
  set_tests_properties(test_native_jail_abuse_corpus_windows PROPERTIES TIMEOUT 120)

  # ---- 008 §9 G1 cross-platform outcome-classification parity (M2 Phase C task C4) ---------------
  add_executable(test_native_jail_parity_windows test_native_jail_parity_windows.cpp)
  target_link_libraries(test_native_jail_parity_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(test_native_jail_parity_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_parity_windows job_object_hostile_child)
  add_test(NAME test_native_jail_parity_windows COMMAND test_native_jail_parity_windows)
  set_tests_properties(test_native_jail_parity_windows PROPERTIES TIMEOUT 60)

  # ---- 008 §9 G3 no-ambient-authority probe (M2 Phase C task C5) ---------------------------------
  add_executable(test_native_jail_ambient_authority_windows test_native_jail_ambient_authority_windows.cpp)
  target_link_libraries(test_native_jail_ambient_authority_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings ws2_32)
  target_compile_definitions(test_native_jail_ambient_authority_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_ambient_authority_windows job_object_hostile_child)
  add_test(NAME test_native_jail_ambient_authority_windows COMMAND test_native_jail_ambient_authority_windows)
  set_tests_properties(test_native_jail_ambient_authority_windows PROPERTIES TIMEOUT 60)

  # ---- 008 §9 G4 teardown-cycle census (M2 Phase C task C6) --------------------------------------
  add_executable(test_native_jail_teardown_cycles_windows test_native_jail_teardown_cycles_windows.cpp)
  target_link_libraries(test_native_jail_teardown_cycles_windows PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings psapi)
  target_compile_definitions(test_native_jail_teardown_cycles_windows PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(test_native_jail_teardown_cycles_windows job_object_hostile_child)
  add_test(NAME test_native_jail_teardown_cycles_windows COMMAND test_native_jail_teardown_cycles_windows)
  # 300 real AppContainer + Job Object launches, plus 5 warm-up -- generous but finite headroom over
  # the measured per-cycle cost, same Machine Safety backstop as the other native_jail tests.
  # RUN_SERIAL: minimizes overlap with sibling tests that share job_object_hostile_child.exe (a
  # real -j4 run surfaced this -- see the test file's own comment on hostile_child_name_lower).
  set_tests_properties(test_native_jail_teardown_cycles_windows PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
endif()

# ---- LinuxNativeJailBackend end-to-end (M2 Phase C task C2's Linux follow-up, 008 §1b/§2/§3) ---
# Needs CAP_SYS_ADMIN (namespaces) and a delegated cgroup v2 root -- run via
# helpers/cgroup_v2_test_setup.sh inside a --privileged container, never assumed available on an
# ordinary CI runner without that setup, so this is opt-in (AGENTENGINE_LINUX_SANDBOX_TESTS), not
# built/run unconditionally the way test_job_object_limits is on Windows.
if(NOT WIN32)
  option(AGENTENGINE_LINUX_SANDBOX_TESTS
    "Build/run the LinuxNativeJailBackend end-to-end test (needs CAP_SYS_ADMIN + delegated cgroup v2)"
    OFF)
  if(AGENTENGINE_LINUX_SANDBOX_TESTS)
    add_executable(hostile_child_posix helpers/hostile_child_posix.cpp)

    add_executable(test_native_jail_backend_linux test_native_jail_backend_linux.cpp)
    target_link_libraries(test_native_jail_backend_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_backend_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_backend_linux hostile_child_posix)
    add_test(NAME test_native_jail_backend_linux
      COMMAND bash -c "source ${CMAKE_CURRENT_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_backend_linux>")
    # Same Machine Safety backstop as the Windows test -- real child processes under real
    # namespace/cgroup isolation, real wall_ms kills under test.
    set_tests_properties(test_native_jail_backend_linux PROPERTIES TIMEOUT 120)

    # ---- 008 §7 abuse-case corpus + §9 G2 positive controls (M2 Phase C task C3, Linux half) -----
    # fs-escape attempt is deliberately NOT included here -- see the test file's own header for the
    # tracked containment gap this surfaced.
    add_executable(test_native_jail_abuse_corpus_linux test_native_jail_abuse_corpus_linux.cpp)
    target_link_libraries(test_native_jail_abuse_corpus_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_abuse_corpus_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_abuse_corpus_linux hostile_child_posix)
    add_test(NAME test_native_jail_abuse_corpus_linux
      COMMAND bash -c "source ${CMAKE_CURRENT_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_abuse_corpus_linux>")
    # The OOM case alone budgets wall_ms=25000 (C2-Linux's own measured worst case) -- 150s total
    # leaves headroom for the other four cases' shorter budgets plus real process/cgroup overhead.
    set_tests_properties(test_native_jail_abuse_corpus_linux PROPERTIES TIMEOUT 150)

    # ---- 008 §9 G1 cross-platform outcome-classification parity (M2 Phase C task C4, Linux half) --
    add_executable(test_native_jail_parity_linux test_native_jail_parity_linux.cpp)
    target_link_libraries(test_native_jail_parity_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_parity_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_parity_linux hostile_child_posix)
    add_test(NAME test_native_jail_parity_linux
      COMMAND bash -c "source ${CMAKE_CURRENT_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_parity_linux>")
    # The oom case alone budgets wall_ms=25000 -- same rationale as the abuse-corpus test above;
    # 90s leaves headroom for the other four cases plus real process/cgroup overhead.
    set_tests_properties(test_native_jail_parity_linux PROPERTIES TIMEOUT 90)

    # ---- 008 §9 G3 no-ambient-authority probe (M2 Phase C task C5, Linux half) -------------------
    # fs axis is deliberately NOT included here -- see the test file's own header for why (same
    # tracked gap as C3's Linux fs-escape omission, GitHub issue #5).
    add_executable(test_native_jail_ambient_authority_linux test_native_jail_ambient_authority_linux.cpp)
    target_link_libraries(test_native_jail_ambient_authority_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_ambient_authority_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_ambient_authority_linux hostile_child_posix)
    add_test(NAME test_native_jail_ambient_authority_linux
      COMMAND bash -c "source ${CMAKE_CURRENT_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_ambient_authority_linux>")
    set_tests_properties(test_native_jail_ambient_authority_linux PROPERTIES TIMEOUT 60)

    # ---- 008 §9 G4 teardown-cycle census (M2 Phase C task C6, Linux half) --------------------------
    add_executable(test_native_jail_teardown_cycles_linux test_native_jail_teardown_cycles_linux.cpp)
    target_link_libraries(test_native_jail_teardown_cycles_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_teardown_cycles_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_teardown_cycles_linux hostile_child_posix)
    add_test(NAME test_native_jail_teardown_cycles_linux
      COMMAND bash -c "source ${CMAKE_CURRENT_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_teardown_cycles_linux>")
    set_tests_properties(test_native_jail_teardown_cycles_linux PROPERTIES TIMEOUT 150)
  endif()
endif()

# ---- Capability bearer-token prove phase (decisions/ADR-005-capability-bearer-tokens-cross-process.md)
# Resolves OQ-3. Design A (capability_token.hpp) correctness + red-team, Design B
# (capability_registry.hpp) red-team, and the verify()-vs-check() cost comparison §5 rests on.
if(WIN32)
  add_executable(test_capability_token_proof test_capability_token_proof.cpp)
  target_link_libraries(test_capability_token_proof PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_capability_token_proof COMMAND test_capability_token_proof)

  add_executable(test_capability_token_redteam test_capability_token_redteam.cpp)
  target_link_libraries(test_capability_token_redteam PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_capability_token_redteam COMMAND test_capability_token_redteam)

  add_executable(test_capability_token_benchmark test_capability_token_benchmark.cpp)
  target_link_libraries(test_capability_token_benchmark PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_capability_token_benchmark COMMAND test_capability_token_benchmark)
endif()

# ---- Worktree content-addressed object store (025 §2, Milestone 3 Phase A1) --------------------
if(WIN32)
  add_executable(test_worktree_object_store test_worktree_object_store.cpp)
  target_link_libraries(test_worktree_object_store PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_object_store COMMAND test_worktree_object_store)
endif()

# ---- Worktree Ref persistence over Quark's Store seam (025 §2, Milestone 3 Phase A2) -----------
# Header-only (core/worktree.hpp's commit_ref/read_ref) -- links only agentengine::core, no
# platform-specific crypto dependency (unlike A1's digest computation).
add_executable(test_worktree_ref_store test_worktree_ref_store.cpp)
target_link_libraries(test_worktree_ref_store PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_worktree_ref_store COMMAND test_worktree_ref_store)

# ---- Worktree sub-worktree sharing modes (025 §3, Milestone 3 Phase B1) ------------------------
# `scratch` mode calls empty_tree_digest() -> compute_digest(), so this needs the same real,
# Windows-only digest implementation A1's own test does (unlike A2's, which never computes a digest).
if(WIN32)
  add_executable(test_worktree_sub_worktree test_worktree_sub_worktree.cpp)
  target_link_libraries(test_worktree_sub_worktree PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_sub_worktree COMMAND test_worktree_sub_worktree)
endif()

# ---- Worktree three-way merge on branch join (025 §4, Milestone 3 Phase B2) ---------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, which need compute_digest() -- same
# Windows-only real-crypto dependency as A1/B1's own tests.
if(WIN32)
  add_executable(test_worktree_merge test_worktree_merge.cpp)
  target_link_libraries(test_worktree_merge PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_merge COMMAND test_worktree_merge)
endif()

# ---- shared-mode staleness note (025 §3/§10 Q2, Milestone 3 Phase B3) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto dependency as A1/B1/B2.
if(WIN32)
  add_executable(test_worktree_staleness test_worktree_staleness.cpp)
  target_link_libraries(test_worktree_staleness PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_staleness COMMAND test_worktree_staleness)
endif()

# ---- Branch-merge concurrency proof (025 §9 G3, Milestone 3 Phase B4) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto dependency as A1/B1/B2/B3.
# Hundreds of simulated-interleaving trials, each doing real SHA-256 work -- bounded (decision 7),
# but still real CPU cost, so this is registered with an explicit timeout rather than CTest's default
# unbounded wait (CLAUDE.md Machine Safety: a runaway proof must not be able to hang the box).
if(WIN32)
  add_executable(test_worktree_branch_concurrency test_worktree_branch_concurrency.cpp)
  target_link_libraries(test_worktree_branch_concurrency PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_branch_concurrency COMMAND test_worktree_branch_concurrency)
  set_tests_properties(test_worktree_branch_concurrency PROPERTIES TIMEOUT 60)
endif()

# ---- Worktree mounts, capability-gated (025 §5, Milestone 3 Phase C1) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto dependency as A1/B1/B2/B3/B4.
if(WIN32)
  add_executable(test_worktree_mount test_worktree_mount.cpp)
  target_link_libraries(test_worktree_mount PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_mount COMMAND test_worktree_mount)
endif()

# ---- Worktree mount<->real-directory sync (025 §5/§7, Milestone 3 Phase F1) ---------------------
# Proves materialize_mount/harvest_mount (src/backends/native_jail/worktree_mount_sync.hpp) against
# the REAL MediatedFileSystemAdapter (ADR-014), not a test double -- needs
# agentengine::mediated_shell_runner for that adapter, same as test_mediated_shell_runner_smoke.
if(WIN32)
  add_executable(test_worktree_mount_sync test_worktree_mount_sync.cpp)
  target_link_libraries(test_worktree_mount_sync PRIVATE
    agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_mount_sync COMMAND test_worktree_mount_sync)
  set_tests_properties(test_worktree_mount_sync PROPERTIES TIMEOUT 60)
endif()

# ---- Worktree mount write quotas (025 §5, Milestone 3 Phase C3) ---------------------------------
# Proves the byte/file-count quota enforcement mount_write (core/worktree.hpp) now performs, against
# the same in-memory content-addressed store as C1 -- no real filesystem needed for this layer.
if(WIN32)
  add_executable(test_worktree_mount_quota test_worktree_mount_quota.cpp)
  target_link_libraries(test_worktree_mount_quota PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_mount_quota COMMAND test_worktree_mount_quota)
endif()

# ---- Worktree mount OS-level path-escape corpus (025 §5, 021 §6 G3, Milestone 3 Phase C2,
# decisions/ADR-014-worktree-mount-path-canonicalization.md) --------------------------------------
# Real Win32 filesystem I/O against a scratch directory under the system temp path: junctions,
# real reparse-point crossing, a real TOCTOU interleaving proven deterministically. Shells out to
# `cmd.exe /c mklink /J` for junction setup (test-only infrastructure), so this needs cmd.exe on
# PATH -- always true on a real Windows install.
if(WIN32)
  add_executable(test_worktree_mount_fs_escape_corpus test_worktree_mount_fs_escape_corpus.cpp)
  target_link_libraries(test_worktree_mount_fs_escape_corpus PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_mount_fs_escape_corpus COMMAND test_worktree_mount_fs_escape_corpus)
  set_tests_properties(test_worktree_mount_fs_escape_corpus PROPERTIES TIMEOUT 60)

  # ---- list_within_mount_root (Milestone 3 Phase G2, 026 §5's agent.files "listing" claim) -------
  add_executable(test_worktree_mount_fs_listdir test_worktree_mount_fs_listdir.cpp)
  target_link_libraries(test_worktree_mount_fs_listdir PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_mount_fs_listdir COMMAND test_worktree_mount_fs_listdir)
  set_tests_properties(test_worktree_mount_fs_listdir PROPERTIES TIMEOUT 60)
endif()

# ---- Worktree mount OS-level path-escape corpus, Linux half (025 §5, 021 §6 G3, Milestone 3
# Phase C4, ADR-014's parity follow-on) ------------------------------------------------------------
# Real Linux filesystem I/O against a scratch directory under $TMPDIR: real symlinks (no special
# privilege needed, unlike Windows junctions), a real TOCTOU interleaving proven deterministically
# the same way the Windows corpus does. No CAP_SYS_ADMIN/cgroup delegation needed (unlike the Linux
# native-jail sandbox tests below) -- this is ordinary unprivileged file I/O, so it runs
# unconditionally under `if(NOT WIN32)`, not gated behind AGENTENGINE_LINUX_SANDBOX_TESTS.
if(NOT WIN32)
  add_executable(test_worktree_mount_fs_escape_corpus_linux test_worktree_mount_fs_escape_corpus_linux.cpp)
  target_link_libraries(test_worktree_mount_fs_escape_corpus_linux PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_mount_fs_escape_corpus_linux COMMAND test_worktree_mount_fs_escape_corpus_linux)
  set_tests_properties(test_worktree_mount_fs_escape_corpus_linux PROPERTIES TIMEOUT 60)
endif()

# ---- Worktree turn-boundary commit and rewind (025 §6, §9 G5, Milestone 3 Phase D1/D2) ----------
# Builds real Blob/Tree fixtures via put_blob/put_tree and one mount_read integration check, same
# real-crypto dependency as A1/B1/B2/B3/B4/C1.
if(WIN32)
  add_executable(test_worktree_turn_commit test_worktree_turn_commit.cpp)
  target_link_libraries(test_worktree_turn_commit PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_turn_commit COMMAND test_worktree_turn_commit)
endif()

# ---- PythonRunner prove-phase gates (decisions/ADR-002-pythonrunner-embedding-and-mediation.md)
# Only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON (CMakeLists.txt) -- these link a real
# CPython import lib and need python3<minor>.dll reachable via PATH at run time (Windows has no
# rpath), wired below via each test's ENVIRONMENT property.
if(AGENTENGINE_BUILD_PYTHON_RUNNER)
  set(AE_PYTHON_TEST_ENV "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}")
  # Every one of these tests spawns a real CPython interpreter and, in some cases, imports numpy/
  # pandas -- bounded per CLAUDE.md's Machine Safety section (a hung or runaway interpreter must
  # not be able to take the dev box down) rather than left to CTest's default (no timeout).

  # Priority item 1: a minimal embedded CPython interpreter actually runs, linked against the
  # configured CPython install, through PythonRunner's real (non-stub) `run()`.
  add_executable(test_python_embed_smoke test_python_embed_smoke.cpp)
  target_link_libraries(test_python_embed_smoke PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_embed_smoke PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_embed_smoke COMMAND test_python_embed_smoke)
  set_tests_properties(test_python_embed_smoke PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase E2's real PythonRunner (mediated_python_runner.{hpp,cpp}) ---------------
  add_executable(test_mediated_python_runner_smoke test_mediated_python_runner_smoke.cpp)
  target_link_libraries(test_mediated_python_runner_smoke PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_mediated_python_runner_smoke PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_mediated_python_runner_smoke COMMAND test_mediated_python_runner_smoke)
  set_tests_properties(test_mediated_python_runner_smoke PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase G1: agent.tools generated-code execution proof (026 §4/§5) --------------
  # Generation-only correctness (no CPython dependency) is test_agent_tools_codegen.cpp, above.
  add_executable(test_mediated_python_runner_agent_tools test_mediated_python_runner_agent_tools.cpp)
  target_link_libraries(test_mediated_python_runner_agent_tools PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_mediated_python_runner_agent_tools PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_mediated_python_runner_agent_tools COMMAND test_mediated_python_runner_agent_tools)
  set_tests_properties(test_mediated_python_runner_agent_tools PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase G2: agent.files/agent.data generated-code execution proof (026 §5) -------
  # Generation-only correctness (no CPython dependency) is test_agent_files_data_codegen.cpp, above.
  add_executable(test_mediated_python_runner_agent_files_data test_mediated_python_runner_agent_files_data.cpp)
  target_link_libraries(test_mediated_python_runner_agent_files_data PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_mediated_python_runner_agent_files_data PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_mediated_python_runner_agent_files_data COMMAND test_mediated_python_runner_agent_files_data)
  set_tests_properties(test_mediated_python_runner_agent_files_data PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase G4: error mapping (026 §3, §9 Q2) execution proof -----------------------
  add_executable(test_mediated_python_runner_error_mapping test_mediated_python_runner_error_mapping.cpp)
  target_link_libraries(test_mediated_python_runner_error_mapping PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_mediated_python_runner_error_mapping PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_mediated_python_runner_error_mapping COMMAND test_mediated_python_runner_error_mapping)
  set_tests_properties(test_mediated_python_runner_error_mapping PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase H1: reference-agent task corpus (026 §8 G1) ------------------------------
  # Hand-authored RecordedChatClient fixtures (tests/fixtures/chat_client/reference_agent/) played
  # back and actually executed through the real MediatedPythonRunner -- needs nlohmann_json (the
  # fixture loader) on top of the usual mediated_python_runner link. Longer timeout: shares the
  # numpy/pandas closure, so pays the same import-time cost as test_python_numpy_pandas_import.
  add_executable(test_reference_agent_task_corpus test_reference_agent_task_corpus.cpp)
  target_link_libraries(test_reference_agent_task_corpus PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings
    nlohmann_json::nlohmann_json)
  target_compile_definitions(test_reference_agent_task_corpus PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}"
    AE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/chat_client/reference_agent")
  add_test(NAME test_reference_agent_task_corpus COMMAND test_reference_agent_task_corpus)
  set_tests_properties(test_reference_agent_task_corpus PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # ---- Milestone 3 Phase H2: transparency-is-not-security containment invariance (026 §1a/§8 G4) --
  add_executable(test_reference_agent_containment_invariance test_reference_agent_containment_invariance.cpp)
  target_link_libraries(test_reference_agent_containment_invariance PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_reference_agent_containment_invariance PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_reference_agent_containment_invariance COMMAND test_reference_agent_containment_invariance)
  set_tests_properties(test_reference_agent_containment_invariance PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- Milestone 3 Phase E4: hostile-corpus/mediation proof (010 §9 G2/G7) -----------------------
  add_executable(test_mediated_python_runner_hostile_corpus test_mediated_python_runner_hostile_corpus.cpp)
  target_link_libraries(test_mediated_python_runner_hostile_corpus PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_mediated_python_runner_hostile_corpus PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_mediated_python_runner_hostile_corpus COMMAND test_mediated_python_runner_hostile_corpus)
  set_tests_properties(test_mediated_python_runner_hostile_corpus PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 2: Layer 0's sys.modules sweep, its measured scope on this target, and the
  # _imp-removal/reimport findings (ADR-002 §6 items 4/7). Talks to CPython directly (no
  # PythonLockdownInterpreter, no meta-path finder) -- see the file's own header comment for why.
  add_executable(test_python_layer0_sweep test_python_layer0_sweep.cpp)
  target_include_directories(test_python_layer0_sweep PRIVATE "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(test_python_layer0_sweep PRIVATE
    agentengine::core "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  target_compile_definitions(test_python_layer0_sweep PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}")
  add_test(NAME test_python_layer0_sweep COMMAND test_python_layer0_sweep)
  set_tests_properties(test_python_layer0_sweep PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 3: the real sys.meta_path finder -- claims A1, A3, plus the §3.4 item 3 / §5.5.2
  # per-call reassertion mechanism (A4).
  add_executable(test_python_meta_path_finder test_python_meta_path_finder.cpp)
  target_link_libraries(test_python_meta_path_finder PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_meta_path_finder PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_meta_path_finder COMMAND test_python_meta_path_finder)
  set_tests_properties(test_python_meta_path_finder PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Priority item 4: claim A2 -- numpy/pandas actually importing and computing under the
  # locked-down interpreter, with the measured real dependency closure explicitly allowlisted.
  # Longer timeout: numpy/pandas's own import-time work (compiled ufunc table setup, etc.) is
  # noticeably slower than the other tests' trivial scripts.
  add_executable(test_python_numpy_pandas_import test_python_numpy_pandas_import.cpp)
  target_link_libraries(test_python_numpy_pandas_import PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_numpy_pandas_import PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}")
  add_test(NAME test_python_numpy_pandas_import COMMAND test_python_numpy_pandas_import)
  set_tests_properties(test_python_numpy_pandas_import PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # Stretch item (§4 claim C1, partial/observational -- see the test file's own header for exactly
  # what is and is not proven): the native PySys_AddAuditHook-installed hook.
  add_executable(test_python_audit_hook test_python_audit_hook.cpp)
  target_link_libraries(test_python_audit_hook PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_audit_hook PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_audit_hook COMMAND test_python_audit_hook)
  set_tests_properties(test_python_audit_hook PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- decisions/ADR-003-caller-aware-import-gating.md prove-phase gates (Design B, revised) ----

  # Claims B1-B8, B10, B11, B13 (primary half): the caller-gated import tier end to end, against
  # the same real numpy/pandas target ADR-002 already measured, with ctypes/_ctypes/winreg/_wmi/
  # _winapi/subprocess moved into caller_gated_modules.
  add_executable(test_python_caller_gated_import test_python_caller_gated_import.cpp)
  target_link_libraries(test_python_caller_gated_import PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_caller_gated_import PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}"
    # B14 (added during this prove pass's own verification, not in the ADR's original §4 table):
    # a trusted package using importlib.import_module() -- not the `import` statement -- for a
    # not-yet-cached gated name. See python_lockdown.cpp's g_importlib_dict comment.
    AE_PYTHON_CALLER_GATED_TRUSTED_PROBE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/python_caller_gated_trusted_probe")
  add_test(NAME test_python_caller_gated_import COMMAND test_python_caller_gated_import)
  set_tests_properties(test_python_caller_gated_import PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 120)

  # Claim B9 (performance): stack walk vs. O(1) allowed-name lookup, N fresh imports each,
  # p50/p99. Uses tiny synthetic fixture modules (tests/fixtures/python_caller_gated_bench/), not
  # numpy/pandas, so the two conditions differ ONLY in which tier decides the name is allowed.
  add_executable(test_python_caller_gated_benchmark test_python_caller_gated_benchmark.cpp)
  target_link_libraries(test_python_caller_gated_benchmark PRIVATE
    agentengine::core agentengine::python_runner agentengine_warnings)
  target_compile_definitions(test_python_caller_gated_benchmark PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_CALLER_GATED_BENCH_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/python_caller_gated_bench")
  add_test(NAME test_python_caller_gated_benchmark COMMAND test_python_caller_gated_benchmark)
  set_tests_properties(test_python_caller_gated_benchmark PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # Claim B13, second half: lockdown_identity_intact() must detect a TrustedLoaderProxy built as a
  # mutable heap type (no Py_TPFLAGS_IMMUTABLETYPE). Needs its OWN compilation of
  # python_lockdown.cpp with AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY defined -- a separate static
  # library, never linked into agentengine_python_runner (the real backend), so the real target's
  # own §3.3.4 construction is never at risk of picking up this test-only branch by accident.
  add_library(agentengine_python_runner_heap_proxy_probe STATIC
    "${CMAKE_SOURCE_DIR}/src/backends/native_jail/python_lockdown.cpp")
  target_include_directories(agentengine_python_runner_heap_proxy_probe PUBLIC
    "${CMAKE_SOURCE_DIR}/src")
  target_include_directories(agentengine_python_runner_heap_proxy_probe PRIVATE
    "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(agentengine_python_runner_heap_proxy_probe PUBLIC agentengine::core)
  target_link_libraries(agentengine_python_runner_heap_proxy_probe PRIVATE
    "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  target_compile_definitions(agentengine_python_runner_heap_proxy_probe PRIVATE
    AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY)

  add_executable(test_python_trusted_loader_proxy_heap_type_negative_control
    test_python_trusted_loader_proxy_heap_type_negative_control.cpp)
  target_link_libraries(test_python_trusted_loader_proxy_heap_type_negative_control PRIVATE
    agentengine::core agentengine_python_runner_heap_proxy_probe agentengine_warnings)
  target_compile_definitions(test_python_trusted_loader_proxy_heap_type_negative_control PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_python_trusted_loader_proxy_heap_type_negative_control
    COMMAND test_python_trusted_loader_proxy_heap_type_negative_control)
  set_tests_properties(test_python_trusted_loader_proxy_heap_type_negative_control PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
endif()
