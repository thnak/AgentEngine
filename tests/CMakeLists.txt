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
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
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
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
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
      "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
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
      "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
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
      "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
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

# ---- ADR-071 compile-fail proof (native-execution family distinctness) -------------------------
# `NativeCapabilityAnnouncer<Ps...>` (src/backends/native_process/native_capability_announcer.hpp)
# must reject composing two providers sharing the same declared family name (e.g. two
# `NativePythonProvider`s) -- otherwise an LLM could be handed two functionally-identical
# native-execution tools with no principled way to choose between them. Header-only; no linking
# needed (the `requires` clause is checked at the point the type alias itself is named, before any
# member function is instantiated). WIN32-gated: this backend's headers are Windows-only today
# (native_providers.hpp includes <windows.h> unconditionally), matching AGENTENGINE_WITH_NATIVE_
# PROCESS's own current platform scope -- independent of whether that CMake option happens to be ON
# for this particular configure, since try_compile() runs in an isolated mini-project that only
# needs the header search path, not the outer project's own build graph.
if(WIN32)
  try_compile(AE_NATIVE_ANNOUNCER_REJECTS_DUPLICATE_FAMILY
    "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/native_announcer_rejects_duplicate_family"
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/native_capability_announcer_rejects_duplicate_family.cpp"
    CXX_STANDARD 23
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/src"
    OUTPUT_VARIABLE AE_NATIVE_ANNOUNCER_REJECTS_DUPLICATE_FAMILY_LOG)
  if(AE_NATIVE_ANNOUNCER_REJECTS_DUPLICATE_FAMILY)
    message(FATAL_ERROR
      "ADR-071 VIOLATED: tests/compile_fail/native_capability_announcer_rejects_duplicate_family.cpp "
      "compiled, but NativeCapabilityAnnouncer<Ps...> must reject composing two providers with the "
      "same declared family name (Ps::name) -- an LLM must never be handed two functionally-identical "
      "native-execution tools. Build log:\n${AE_NATIVE_ANNOUNCER_REJECTS_DUPLICATE_FAMILY_LOG}")
  endif()

  try_compile(AE_NATIVE_ANNOUNCER_DISTINCT_FAMILIES_POSITIVE_CONTROL
    "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/native_announcer_distinct_families_positive_control"
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/native_capability_announcer_distinct_families_positive_control.cpp"
    CXX_STANDARD 23
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/src"
    OUTPUT_VARIABLE AE_NATIVE_ANNOUNCER_DISTINCT_FAMILIES_POSITIVE_CONTROL_LOG)
  if(NOT AE_NATIVE_ANNOUNCER_DISTINCT_FAMILIES_POSITIVE_CONTROL)
    message(FATAL_ERROR
      "ADR-071 positive control FAILED: tests/compile_fail/"
      "native_capability_announcer_distinct_families_positive_control.cpp must compile (four DISTINCT "
      "families composing together is the intended, legitimate path) but did not. "
      "Build log:\n${AE_NATIVE_ANNOUNCER_DISTINCT_FAMILIES_POSITIVE_CONTROL_LOG}")
  endif()
  message(STATUS
    "ADR-071 compile-fail proof: OK (NativeCapabilityAnnouncer rejects a duplicated native-execution "
    "family; distinct families still compose)")
endif()

# ---- Keep assert() live in every configuration, Release included --------------------------------
# 13 files here (195 assert() calls) use bare <cassert> rather than the AE_CHECK/g_failures harness:
# smoke_vocabulary, test_job_object_limits, test_native_jail_runner_stubs, test_python_audit_hook,
# test_python_caller_gated_{benchmark,import}, test_python_embed_smoke, test_python_layer0_sweep,
# test_python_meta_path_finder, test_python_numpy_pandas_import,
# test_python_trusted_loader_proxy_heap_type_negative_control, test_real_filesystem_adapter,
# test_recorded_chat_client.
#
# CMake's Release/RelWithDebInfo flags carry -DNDEBUG, which turns every one of those into a no-op --
# so the CI jobs that build Release (Windows/MSVC, Windows/clang-cl, Linux/gcc-14) were running
# bodies that cannot fail. That is the exact thing CLAUDE.md forbids ("a test that cannot fail proves
# nothing"), and it covered real security proofs: the caller-gated import gate (45 asserts), the
# audit hook, the Job-object limits, and a file whose name is literally
# ..._negative_control.cpp. gcc's -Wunused-variable is what exposed it -- four locals in
# test_recorded_chat_client.cpp had no remaining reader once the asserts were preprocessed away.
#
# Dropping NDEBUG for tests/ only is the narrow fix: production targets keep their Release semantics
# untouched, and the assertions become load-bearing everywhere they are built.
#
# The define is STRIPPED from this directory's config flags rather than countered with a
# `/UNDEBUG`-`-UNDEBUG` compile option. Both undefine it, but appending the counter-flag makes cl
# itself emit `command line warning D9025: overriding '/DNDEBUG' with '/UNDEBUG'` on every single
# translation unit -- trading 195 dead assertions for a few thousand lines of new warning, when the
# rule here is that warnings get fixed rather than accumulated. Editing the variables at directory
# scope leaves no contradictory pair on the command line for cl to complain about.
# Three places NDEBUG can arrive from, and an earlier version of this block only handled the first:
#   1. CMAKE_<LANG>_FLAGS_<CONFIG>          -- CMake's own Release/RelWithDebInfo/MinSizeRel defaults
#   2. CMAKE_<LANG>_FLAGS                   -- seeded by the CXXFLAGS/CFLAGS environment variables and
#                                              by -DCMAKE_CXX_FLAGS=..., both entirely ordinary in a
#                                              packaging or toolchain-file build
#   3. the directory COMPILE_DEFINITIONS property -- an add_compile_definitions(NDEBUG) above us
# Missing (2) meant a perfectly standard configuration silently restored all 195 dead assertions,
# which is precisely the failure this block exists to prevent, so all three are handled.
foreach(ae_lang C CXX)
  foreach(ae_flags_var CMAKE_${ae_lang}_FLAGS
                        CMAKE_${ae_lang}_FLAGS_RELEASE
                        CMAKE_${ae_lang}_FLAGS_RELWITHDEBINFO
                        CMAKE_${ae_lang}_FLAGS_MINSIZEREL)
    string(REPLACE "/DNDEBUG" "" ${ae_flags_var} "${${ae_flags_var}}")
    string(REPLACE "-DNDEBUG" "" ${ae_flags_var} "${${ae_flags_var}}")
  endforeach()
endforeach()
get_directory_property(ae_dir_defs COMPILE_DEFINITIONS)
if(ae_dir_defs)
  list(REMOVE_ITEM ae_dir_defs NDEBUG)
  set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${ae_dir_defs}")
endif()

add_executable(smoke_vocabulary smoke_vocabulary.cpp)
target_link_libraries(smoke_vocabulary PRIVATE agentengine::core agentengine_warnings)

add_test(NAME smoke_vocabulary COMMAND smoke_vocabulary)

# ---- Milestone 4 Phase B2 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# HistoryProvider<Window<N>>, the first real ContextProvider conformer -- standalone (unit) and
# wired end to end through AgentSession's real turn loop.
add_executable(test_history_provider test_history_provider.cpp)
target_link_libraries(test_history_provider PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_history_provider COMMAND test_history_provider)

# ---- Milestone 4 Phase B3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Generalized N-contributor assemble_context(): per-contributor token budgets, deterministic
# oldest-first drop order, drops recorded, purity/replay across repeated runs.
add_executable(test_context_assembly test_context_assembly.cpp)
target_link_libraries(test_context_assembly PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_context_assembly COMMAND test_context_assembly)

# ---- decisions/ADR-066-context-provider-attribution-provenance.md's prove phase: per-contributor
# attribution stamping on Message/ToolDescriptor, and the narrower (not blanket) content_origin::user
# forgery check, both applied structurally at assemble_context()'s own seam.
add_executable(test_context_provenance test_context_provenance.cpp)
target_link_libraries(test_context_provenance PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_context_provenance COMMAND test_context_provenance)

# ---- decisions/ADR-066-*.md §7 residual: Message::attribution now survives a round-trip through
# rt/message_codec.hpp's JSON codec (previously confirmed absent).
add_executable(test_message_codec_attribution test_message_codec_attribution.cpp)
target_link_libraries(test_message_codec_attribution PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_message_codec_attribution COMMAND test_message_codec_attribution)

# ---- decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md's prove phase: the turn/
# pre_model interception point (TurnContext, ToolSurfaceView, redact_subspan(), Compactor<N>), wired
# as its own standalone, tested mechanism -- not yet threaded into rt::AgentSession::run_rounds()
# (named residual, matching middleware.hpp's own precedent for the run/tool-call points).
add_executable(test_turn_middleware test_turn_middleware.cpp)
target_link_libraries(test_turn_middleware PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_turn_middleware COMMAND test_turn_middleware)

# ---- decisions/ADR-069-content-triggered-model-response-replay.md's prove phase: ContentReplayGateway
# -- a settled response's content-triggered discard-and-retry, wrapping any ModelCallGatewayLike,
# bounded both per-trigger-site and across a session's lifetime independently.
add_executable(test_content_replay_gateway test_content_replay_gateway.cpp)
target_link_libraries(test_content_replay_gateway PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_content_replay_gateway COMMAND test_content_replay_gateway)

# ---- ComposedContextProvider<Ms...> (core/composed_context_provider.hpp) -- generalizes
# history_and_skills_provider.hpp's fixed two-provider HistoryAndSkillsProvider<H,S> to an arbitrary
# Ms... pack via the SAME assemble_context() B3 already proved, and wires it into rt::AgentSession's
# single provider template slot end to end (N == 3 providers, deliberately > 2).
add_executable(test_composed_context_provider test_composed_context_provider.cpp)
target_link_libraries(test_composed_context_provider PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_composed_context_provider COMMAND test_composed_context_provider)

# ---- Skills (009 §8), Phase 1: SKILL.md parser (§8a) -- pure text-in/value-out, no VFS dependency --
add_executable(test_skill_parser test_skill_parser.cpp)
target_link_libraries(test_skill_parser PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_skill_parser COMMAND test_skill_parser)

# ---- Skills (009 §8): InlineSkillSource + builtin_skills.hpp's one shipped example (§8f) ---------
add_executable(test_skill_source_inline test_skill_source_inline.cpp)
target_link_libraries(test_skill_source_inline PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_skill_source_inline COMMAND test_skill_source_inline)

# ---- Skills (009 §8): DiskSkillSource against a real host temp directory -- the "load from disk" --
# half of the MAF-parity ask (InlineSkillSource is the "inline skill" half, tested above).
add_executable(test_skill_source_disk test_skill_source_disk.cpp)
target_link_libraries(test_skill_source_disk PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_skill_source_disk COMMAND test_skill_source_disk)

# ---- Milestone 4 Phase B4 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# HistoryProvider<Summarize<N, SummarizerT>> -- the second named compaction strategy (005 §4),
# proven against a mock summarizer ChatClient (decision 8's established precedent).
add_executable(test_history_provider_summarize test_history_provider_summarize.cpp)
target_link_libraries(test_history_provider_summarize PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_history_provider_summarize COMMAND test_history_provider_summarize)

# ---- ADR-037 Phase 2 -- porting test_agent_session_checkpoint.cpp's D1 (restart doesn't remint a
# run_id) and D2 (CheckpointCadence<N> fires at the exact expected turns over a real 7-turn
# sequence, and the durable/live-state lag is honestly observable) onto rt::AgentSession directly.
# Split into two files since the two claims are genuinely independent -- supersedes the old file.
add_executable(test_rt_agent_session_checkpoint_restart test_rt_agent_session_checkpoint_restart.cpp)
target_link_libraries(test_rt_agent_session_checkpoint_restart PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_checkpoint_restart COMMAND test_rt_agent_session_checkpoint_restart)

add_executable(test_rt_agent_session_checkpoint_cadence test_rt_agent_session_checkpoint_cadence.cpp)
target_link_libraries(test_rt_agent_session_checkpoint_cadence PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_checkpoint_cadence COMMAND test_rt_agent_session_checkpoint_cadence)

# ---- Milestone 4 Phase F1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md) ----
# Idempotency key derivation {run_id, turn_index, call_index, argument_digest}, wired into the
# real (M2) tool_pipeline.hpp invocation pipeline.
add_executable(test_idempotency_key test_idempotency_key.cpp)
target_link_libraries(test_idempotency_key PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_idempotency_key COMMAND test_idempotency_key)

# ---- ADR-037 -- agentengine::rt::EffectJournalEntry (019 §3's effect journaling: intent before
# execution, outcome after), ported off the old Quark quark::Store/EventLog shape onto
# rt::AppendLogStore -- name-scoped by session_id rather than sharing a literal Store/ActorId slot
# with the session's own checkpoint (rt::SessionStore and rt::AppendLogStore are structurally
# different interfaces; see effect_journal.hpp's own banner). Supersedes test_effect_journal.cpp,
# whose F2-R1..R7 claims are ported (J1-J7) and whose F2-R8 is replaced with a genuinely analogous
# J8 (two sessions' journals in the same store instance never collide).
add_executable(test_rt_effect_journal test_rt_effect_journal.cpp)
target_link_libraries(test_rt_effect_journal PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_effect_journal COMMAND test_rt_effect_journal)

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

# ---- ADR-063 (retrieval-augmented ContextProvider shape) -- foundation layer, Proposed status ----
# §2.6b / §3 claim 6: neutralize_forged_provenance_markers() (core/provenance_marker.hpp), the
# marker-forgery defense generalized out of memory_provider.hpp's own gap-17 fix.
if(WIN32)
  add_executable(test_provenance_marker test_provenance_marker.cpp)
  target_link_libraries(test_provenance_marker PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_provenance_marker COMMAND test_provenance_marker)
endif()

# §2.3 / §4 finding 7: BruteForceCosineIndex (core/vector_index.hpp) -- ranking correctness, contract
# rejection, and the deterministic score-desc/id-asc tie-break.
if(WIN32)
  add_executable(test_vector_index test_vector_index.cpp)
  target_link_libraries(test_vector_index PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_vector_index COMMAND test_vector_index)

  add_executable(test_vector_index_benchmark test_vector_index_benchmark.cpp)
  target_link_libraries(test_vector_index_benchmark PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_vector_index_benchmark COMMAND test_vector_index_benchmark)
endif()

# §2.6a / §3 claim 5: rag_corpus_key()/rag_corpus_mount() (core/corpus_scope.hpp) -- multi-tenant
# corpus isolation, mirroring test_memory_cross_tenant_isolation.cpp's proven shape.
if(WIN32)
  add_executable(test_corpus_scope test_corpus_scope.cpp)
  target_link_libraries(test_corpus_scope PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_corpus_scope COMMAND test_corpus_scope)
endif()

# §2.4 / §4 findings 1-2: CorpusChunkRecord (core/corpus_chunk.hpp) -- the shared citation-metadata
# contract CorpusSource (writer) and VectorRagContextProvider (reader) both build against.
if(WIN32)
  add_executable(test_corpus_chunk test_corpus_chunk.cpp)
  target_link_libraries(test_corpus_chunk PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_corpus_chunk COMMAND test_corpus_chunk)
endif()

# §2.4/§2.4b, closes §4 findings 2/8/9 for its own call site: DiskCorpusSource + RecursiveChunker
# (core/corpus_source.hpp) -- folder-mount ingestion, re-mount dedup, chunking.
if(WIN32)
  add_executable(test_corpus_source test_corpus_source.cpp)
  target_link_libraries(test_corpus_source PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_corpus_source COMMAND test_corpus_source)
endif()

# §2.1/§2.1b/§2.6a/§2.6b, closes the "full claim" gap for §3 claims 5/6: VectorRagContextProvider
# (core/vector_rag_context_provider.hpp) -- the real ContextProvider conformer.
if(WIN32)
  add_executable(test_vector_rag_context_provider test_vector_rag_context_provider.cpp)
  target_link_libraries(test_vector_rag_context_provider PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_vector_rag_context_provider COMMAND test_vector_rag_context_provider)
endif()


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

# ---- ADR-047 (docs/planning/2026-08-10-full-codebase-adr-gap-audit.md gap 18) -----------------
# Proves the real product ranking formula (salience x recency x keyword) directly against
# memory_rank_score() -- recency actually participates, is bounded (not an unbounded raw SeqNo),
# and a zero-salience freshly-extracted item is not permanently rank-zero.
if(WIN32)
  add_executable(test_memory_ranking_formula test_memory_ranking_formula.cpp)
  target_link_libraries(test_memory_ranking_formula PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_memory_ranking_formula COMMAND test_memory_ranking_formula)
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

# ---- cap::NativeExec (decisions/ADR-071-native-unsandboxed-process-execution-providers.md) ------
# Header-only (trust/capability.hpp), core-tier -- links only agentengine::core.
add_executable(test_native_exec_capability test_native_exec_capability.cpp)
target_link_libraries(test_native_exec_capability PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_native_exec_capability COMMAND test_native_exec_capability)

# ---- native_process backend (decisions/ADR-071-native-unsandboxed-process-execution-providers.md)
# Only built when AGENTENGINE_WITH_NATIVE_PROCESS is explicitly turned ON (root CMakeLists.txt) --
# matching AGENTENGINE_BUILD_PYTHON_RUNNER/AGENTENGINE_WITH_WASM's own gating pattern. Windows-only
# for now (native_process_spawn/native_worktree_bridge have no POSIX implementation yet, a named,
# honest gap -- see root CMakeLists.txt's own AGENTENGINE_WITH_NATIVE_PROCESS block).
if(AGENTENGINE_WITH_NATIVE_PROCESS AND WIN32)
  add_executable(test_native_process_spawn test_native_process_spawn.cpp)
  target_link_libraries(test_native_process_spawn PRIVATE
    agentengine::native_process_backend agentengine_warnings)
  add_test(NAME test_native_process_spawn COMMAND test_native_process_spawn)

  add_executable(test_native_path_scan test_native_path_scan.cpp)
  target_link_libraries(test_native_path_scan PRIVATE
    agentengine::native_process_backend agentengine_warnings)
  add_test(NAME test_native_path_scan COMMAND test_native_path_scan)

  add_executable(test_native_worktree_bridge test_native_worktree_bridge.cpp)
  target_link_libraries(test_native_worktree_bridge PRIVATE
    agentengine::native_process_backend agentengine_warnings)
  add_test(NAME test_native_worktree_bridge COMMAND test_native_worktree_bridge)

  add_executable(test_native_providers test_native_providers.cpp)
  target_link_libraries(test_native_providers PRIVATE
    agentengine::core agentengine::native_process_backend agentengine_warnings)
  add_test(NAME test_native_providers COMMAND test_native_providers)

  add_executable(test_native_capability_announcer test_native_capability_announcer.cpp)
  target_link_libraries(test_native_capability_announcer PRIVATE
    agentengine::core agentengine::native_process_backend agentengine_warnings)
  add_test(NAME test_native_capability_announcer COMMAND test_native_capability_announcer)
endif()

# ---- agent.spawn depth/budget bound (decisions/ADR-006-agent-spawn-depth-budget-bound.md) -------
# Header-only (trust/spawn_budget.hpp), core-tier (std only, no crypto -- in-process, never crosses
# a process boundary the way trust/capability_token.hpp's ADR-005 token does) -- links only
# agentengine::core.
add_executable(test_spawn_budget test_spawn_budget.cpp)
target_link_libraries(test_spawn_budget PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_spawn_budget COMMAND test_spawn_budget)

# ---- agent.spawn budget wiring + child capability minting (items 4/5,
# docs/planning/agent-spawn-runtime-design-draft.md; OpenQuestions.md OQ-14) ----------------------
# Header-only (trust/agent_spawn_capability.hpp), core-tier -- links agentengine::core (which also
# carries rt/spawn_cost_budget.hpp, ADR-031/ADR-037's AsyncMutex-serialized token pool this test
# drives directly alongside trust::SpawnBudget/ADR-006).
add_executable(test_agent_spawn_capability test_agent_spawn_capability.cpp)
target_link_libraries(test_agent_spawn_capability PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_spawn_capability COMMAND test_agent_spawn_capability)

# ---- agent.spawn's nested-run invocation mechanism (item 2, docs/planning/
# agent-spawn-runtime-design-draft.md §4.2; OpenQuestions.md OQ-14) ------------------------------
# Header-only (rt/agent_spawn_child_run.hpp), rt-tier -- links agentengine::core (item 4/5's already-
# landed trust/agent_spawn_capability.hpp) and rt::AgentSession (agent_session.hpp) directly.
add_executable(test_rt_agent_spawn_child_run test_rt_agent_spawn_child_run.cpp)
target_link_libraries(test_rt_agent_spawn_child_run PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_spawn_child_run COMMAND test_rt_agent_spawn_child_run)

# ---- agent.spawn's dynamic sub-worktree minting (item 3, docs/planning/
# agent-spawn-runtime-design-draft.md §4.3; OpenQuestions.md OQ-14) ------------------------------
# core/agent_spawn_worktree.hpp calls compute_digest() (derive_spawn_child_id) -- same Windows-only
# real-crypto dependency every other worktree test above (test_workflow_worktree_scoping et al.)
# already needs; also links item 2 (agentengine::core carries rt/agent_spawn_child_run.hpp) for T11's
# end-to-end wiring proof.
if(WIN32)
  add_executable(test_agent_spawn_worktree test_agent_spawn_worktree.cpp)
  target_link_libraries(test_agent_spawn_worktree PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_agent_spawn_worktree COMMAND test_agent_spawn_worktree)
endif()

# ---- agent.spawn's REAL, wired, end-to-end call path (item 1, docs/planning/
# agent-spawn-runtime-design-draft.md §2/§4.1; OpenQuestions.md OQ-14) ---------------------------
# Header-only (rt/agent_spawn.hpp), rt-tier -- composes items 2/3/4/5 above through the REAL
# Tool<>-conforming AgentSpawnTool/AgentSpawnToolProvider/perform_agent_spawn() surface. Same
# WIN32-only compute_digest dependency as test_agent_spawn_worktree (core/agent_spawn_worktree.hpp's
# derive_spawn_child_id() is reached via SpawnPump::process(), not called directly by this file, but
# still links against it).
if(WIN32)
  add_executable(test_rt_agent_spawn test_rt_agent_spawn.cpp)
  target_link_libraries(test_rt_agent_spawn PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_rt_agent_spawn COMMAND test_rt_agent_spawn)
endif()

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

# ---- ADR-061 §10b claim 4 (decisions/ADR-061-host-provided-inbound-transport.md, Tier 1 prove -------
# phase): the MCP conformance client's resolve_host/resolve_and_validate two-way SSRF control. Written
# for real -- tools/mcp_conformance_client.cpp's own top comment referred to this file before it
# existed.
add_executable(test_mcp_conformance_transport test_mcp_conformance_transport.cpp)
target_link_libraries(test_mcp_conformance_transport PRIVATE agentengine::net_egress_proxy agentengine_warnings)
add_test(NAME test_mcp_conformance_transport COMMAND test_mcp_conformance_transport)

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

  # ---- ADR-023 Phase 1: response-format codec (decisions/ADR-023-response-format-codec-seam.md) ----
  # Pure logic (core/response_format_codec.hpp) proven directly against real Harmony/DeepSeek/Hermes/
  # <think> example strings -- no network, no ChatClient backend involved, only core content types.
  add_executable(test_response_format_codec test_response_format_codec.cpp)
  target_link_libraries(test_response_format_codec PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_response_format_codec COMMAND test_response_format_codec)

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

  # ---- ADR-063 §2.5: OpenAIEmbedder (protocol/openai/embedder.hpp) -- offline-provable surface --
  # request/response construction and embed_batch()'s two synchronous pre-network gates. No server,
  # runs unconditionally, mirrors test_openai_chat_client_translation.cpp's own offline split.
  add_executable(test_openai_embedder test_openai_embedder.cpp)
  target_link_libraries(test_openai_embedder PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_openai_embedder COMMAND test_openai_embedder)

  # ---- ADR-063 §3 claim 4's own disproof test: OpenAIEmbedder against REAL api.openrouter.ai -----
  add_executable(test_openai_embedder_openrouter_live_e2e test_openai_embedder_openrouter_live_e2e.cpp)
  target_link_libraries(test_openai_embedder_openrouter_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_openai_embedder_openrouter_live_e2e COMMAND test_openai_embedder_openrouter_live_e2e)
  set_tests_properties(test_openai_embedder_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- OpenAIChatClient against REAL OpenRouter, with a CONCRETELY NAMED OpenAI model (not the
  # routing alias test_openrouter_live_e2e.cpp uses) -- see this file's own top comment for why that
  # distinction is the one new claim it adds.
  add_executable(test_openai_chat_client_openrouter_live_e2e test_openai_chat_client_openrouter_live_e2e.cpp)
  target_link_libraries(test_openai_chat_client_openrouter_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_openai_chat_client_openrouter_live_e2e COMMAND test_openai_chat_client_openrouter_live_e2e)
  set_tests_properties(test_openai_chat_client_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  add_executable(test_llamacpp_live_e2e test_llamacpp_live_e2e.cpp)
  target_link_libraries(test_llamacpp_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_llamacpp_live_e2e COMMAND test_llamacpp_live_e2e)
  set_tests_properties(test_llamacpp_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- 004 §3's OpenAI-compatible backend against a REAL, LOCAL Ollama server -- the other local
  # server that section's table names by name, alongside llama.cpp above.
  add_executable(test_ollama_live_e2e test_ollama_live_e2e.cpp)
  target_link_libraries(test_ollama_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_ollama_live_e2e COMMAND test_ollama_live_e2e)
  set_tests_properties(test_ollama_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # A real AgentSession (not the raw ChatClient layer above) driven through a genuinely hard,
  # multi-tool, multi-turn, forced-parallel-tool-call conversation against a real remote model. Also
  # the live proof for two real bugs this file's own prerequisite fixed (see its own top comment):
  # AgentSession silently dropping declared tools from its outbound request, and the OpenAI backend
  # mishandling a Message carrying multiple ToolResult items (the parallel-tool-call reply shape).
  # ADR-037: ported off quark::TestKit<Session> onto agentengine::rt::AgentSession directly --
  # supersedes the old test_agent_session_live_multitool_e2e.cpp (removed).
  add_executable(test_rt_agent_session_live_multitool_e2e test_rt_agent_session_live_multitool_e2e.cpp)
  target_link_libraries(test_rt_agent_session_live_multitool_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_rt_agent_session_live_multitool_e2e COMMAND test_rt_agent_session_live_multitool_e2e)
  set_tests_properties(test_rt_agent_session_live_multitool_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ADR-029's suspend-for-approval mechanism (HITL), driven by a REAL model's own tool calls instead
  # of a scripted ChatClientT (every offline suspend/approval test uses a scripted one). Hunts for
  # bugs in the suspend/resume/approval-surface machinery itself: a misleading approval_requested
  # fan-out over a mixed gated/ungated round (BUG-1), collateral denial of an ungated call sharing a
  # round with a gated one (BUG-2), an approval surface that exposes no tool_name/arguments for a
  # human to actually decide on (BUG-3), plus softer "mid-conversation drop" (does history_ survive a
  # real suspend/resume across multiple start_run() calls) and "no-explanation" (does a real model's
  # own reply actually explain a denial to the user) probes. See the file's own top comment.
  add_executable(test_rt_agent_session_hitl_live_e2e test_rt_agent_session_hitl_live_e2e.cpp)
  target_link_libraries(test_rt_agent_session_hitl_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_rt_agent_session_hitl_live_e2e COMMAND test_rt_agent_session_hitl_live_e2e)
  set_tests_properties(test_rt_agent_session_hitl_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

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

  # ---- OQ-19: the graph-layer half of the agent-executor CapabilitySet gate -- Executor::
  # capability_ceiling, check_workflow_executable(wf, contexts)'s contexts-aware overload, and the
  # YAML compiler's explicit refusal of an authored capability_ceiling key (I6: refused loudly, never
  # silently dropped). Pure data + the shared validator, no rt:: -- default suite.
  add_executable(test_workflow_agent_executor_gate test_workflow_agent_executor_gate.cpp)
  target_link_libraries(test_workflow_agent_executor_gate PRIVATE agentengine::core agentengine_warnings)
  add_test(NAME test_workflow_agent_executor_gate COMMAND test_workflow_agent_executor_gate)

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

  # ---- ADR-037 Phase 2 -- porting test_agent_session_real_backend(.cpp) (ADR-018: a REAL ChatClient
  # backend driven through a live AgentSession turn loop) and test_agent_session_skills_real_backend
  # (.cpp) (009 §8 + ADR-018: HistoryAndSkillsProvider through a real backend) onto
  # agentengine::rt::AgentSession directly, consolidated into ONE file (both share the identical
  # canned-local-HTTP-server shape) -- supersedes both. Still Windows-only (needs
  # agentengine::worktree_store's real compute_digest for the skills half) and still
  # AGENTENGINE_WITH_HTTPS-gated (guarded inside the .cpp itself, so this registration is
  # unconditional -- the file's own #else main(){return 0;} makes it a no-op test when HTTPS is off).
  if(WIN32)
    add_executable(test_rt_agent_session_real_backend test_rt_agent_session_real_backend.cpp)
    target_link_libraries(test_rt_agent_session_real_backend PRIVATE
      agentengine::core agentengine::provider_http_client agentengine::worktree_store agentengine_warnings
      MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    add_test(NAME test_rt_agent_session_real_backend COMMAND test_rt_agent_session_real_backend)

    # ---- Skills (009 §8), LIVE: the same HistoryAndSkillsProvider composition driven through a
    # two-turn conversation (greeting, then "use your code execution skill") against a REAL remote
    # model over OpenRouter -- the behavioural proof that a mounted skill's advertisement actually
    # reaches and influences a real model's real tool selection, not just the outbound wire bytes
    # (test_agent_session_skills_real_backend.cpp's own canned-server scope).
    # ADR-037: ported off quark::TestKit<Session> onto agentengine::rt::AgentSession directly --
    # supersedes the old test_agent_session_skills_live_e2e.cpp (removed).
    add_executable(test_rt_agent_session_skills_live_e2e test_rt_agent_session_skills_live_e2e.cpp)
    target_link_libraries(test_rt_agent_session_skills_live_e2e PRIVATE
      agentengine::core agentengine::provider_http_client agentengine::worktree_store agentengine_warnings
      MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    add_test(NAME test_rt_agent_session_skills_live_e2e COMMAND test_rt_agent_session_skills_live_e2e)
    set_tests_properties(test_rt_agent_session_skills_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

    # ---- Farm-field-advisory (009 §8 + ADR-024's Phase 3 addendum), LIVE: a NEW skill/tool pair
    # purpose-built for this file (farm-field-advisory -> check_field_weather), not a reuse of the
    # builtin five or of execute_code -- a rice-farmer scenario driven through the same
    # advertise -> mount_skill -> next-turn-unlocked-tool round-trip as the skills test above, against
    # a REAL remote model over OpenRouter. See the .cpp's own top comment for the full rationale.
    add_executable(test_rt_agent_session_farm_advisory_live_e2e test_rt_agent_session_farm_advisory_live_e2e.cpp)
    target_link_libraries(test_rt_agent_session_farm_advisory_live_e2e PRIVATE
      agentengine::core agentengine::provider_http_client agentengine::worktree_store agentengine_warnings
      MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    add_test(NAME test_rt_agent_session_farm_advisory_live_e2e COMMAND test_rt_agent_session_farm_advisory_live_e2e)
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
      add_executable(test_rt_agent_session_farm_ops_live_e2e test_rt_agent_session_farm_ops_live_e2e.cpp)
      target_link_libraries(test_rt_agent_session_farm_ops_live_e2e PRIVATE
        agentengine::core agentengine::provider_http_client agentengine::worktree_store
        agentengine::python_runner agentengine_warnings
        MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
      target_compile_definitions(test_rt_agent_session_farm_ops_live_e2e PRIVATE
        AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
      add_test(NAME test_rt_agent_session_farm_ops_live_e2e COMMAND test_rt_agent_session_farm_ops_live_e2e)
      set_tests_properties(test_rt_agent_session_farm_ops_live_e2e PROPERTIES
        # 480s, not the other live tests' 300s: this file's own max_turns is intentionally unbounded
        # (an ordinary open-ended conversation, not a fixed-step workflow -- see the .cpp's own
        # comment at the initialize() call site), so ctest's TIMEOUT is the only outer bound on a
        # session that occasionally needs several extra rounds to recover and converge.
        LABELS "live-network" TIMEOUT 480
        ENVIRONMENT "PATH=${AGENTENGINE_PYTHON_DLL_DIR};$ENV{PATH}")
    endif()
  endif()

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
  # ADR-037: ported off quark::TestKit<Session>/core::AgentSession onto agentengine::rt::AgentSession
  # for the checkpoint leg (5) -- supersedes the old test_secret_hygiene_canary_scan.cpp (removed).
  add_executable(test_rt_secret_hygiene_canary_scan test_rt_secret_hygiene_canary_scan.cpp)
  target_link_libraries(test_rt_secret_hygiene_canary_scan PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_rt_secret_hygiene_canary_scan COMMAND test_rt_secret_hygiene_canary_scan)

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

  # ---- QuickstartSessionBuilder (core/session_builder.hpp), promoted from prototype after three
  # red-team rounds -- see docs/planning/quickstart-session-builder-design-draft.md. Covers build()-time
  # capability/secret validation and the Bundle Store/CapabilitySet lifetime-across-a-move proof. No
  # .ask()/start_run() call, so no real network exchange happens and OpenAIChatClient::chat()'s body is
  # never instantiated -- linked against provider_http_client only because session_builder.hpp's own
  # file-wide AGENTENGINE_WITH_HTTPS guard requires the real OpenAIChatClient/AnthropicChatClient types
  # to be visible at all, same reason test_json_schema_described above needs the same link with no live
  # network call either.
  add_executable(test_session_builder test_session_builder.cpp)
  target_link_libraries(test_session_builder PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings)
  add_test(NAME test_session_builder COMMAND test_session_builder)

  # ---- unified-streaming-design-draft.md §4 (Piece D), live proof: Bundle::ask()/ask_stream() against
  # REAL OpenRouter -- the one surface this facade's own history names as out of scope until now ("a
  # live, real-network proof of .ask() end to end is separately scoped future work"). Needs the real
  # MbedTLS-backed HTTPS stack, unlike its offline sibling above.
  add_executable(test_session_builder_openrouter_live_e2e test_session_builder_openrouter_live_e2e.cpp)
  target_link_libraries(test_session_builder_openrouter_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  add_test(NAME test_session_builder_openrouter_live_e2e COMMAND test_session_builder_openrouter_live_e2e)
  set_tests_properties(test_session_builder_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)
endif()

# ---- Gap-3 fix (2026-08-14, decisions/ADR-045-*.md) -- core/json_schema_validator.hpp, proving
# 006 §8 G2 (schema violation -> structured error) and 015 §7 G2 (cyclic $ref rejected). No HTTPS
# dependency (unlike its neighbors above) -- deliberately outside that conditional block.
add_executable(test_json_schema_validator test_json_schema_validator.cpp)
target_link_libraries(test_json_schema_validator PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_json_schema_validator COMMAND test_json_schema_validator)

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

# decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md's prove phase.
# QuarantineSecretStore's content-addressed naming reuses trust::hmac_sha256 (trust/hmac.cpp,
# ADR-021) rather than a second, unaudited digest implementation -- that primitive lives in the
# Windows-only agentengine::capability_token library (CMakeLists.txt's own "Cross-process capability
# tokens -- Windows only" section), so this test needs the same link, same as that section's own
# consumers (capability_token.cpp/bearer_token.cpp).
if(WIN32)
  add_executable(test_secret_quarantine test_secret_quarantine.cpp)
  target_link_libraries(test_secret_quarantine PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_secret_quarantine COMMAND test_secret_quarantine)
endif()

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

# ---- Middleware<Ms...>, model-call interception point (002 §5, ADR-033), proven through
# MiddlewareModelCallGateway (ADR-036) -- was test_middleware_chat_client.cpp/MiddlewareChatClient
# until both were removed 2026-08-12.
add_executable(test_middleware_model_call_gateway test_middleware_model_call_gateway.cpp)
target_link_libraries(test_middleware_model_call_gateway PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_middleware_model_call_gateway COMMAND test_middleware_model_call_gateway)

# ---- ADR-037 Phase 2 -- per-run TokenBudget<N> enforcement, ported onto rt::AgentSession directly.
# Claim (2) (over-budget fails closed) was already covered by test_rt_agent_session.cpp's S4 and
# test_rt_agent_session_streaming_and_events.cpp's A4b -- NOT re-ported; this file covers the three
# claims that were genuinely uncovered (under-budget succeeds, unbounded default, per-run reset).
add_executable(test_rt_agent_session_token_budget test_rt_agent_session_token_budget.cpp)
target_link_libraries(test_rt_agent_session_token_budget PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_token_budget COMMAND test_rt_agent_session_token_budget)

# ---- ADR-037 Phase 2 -- ADR-027's real, internal tool-call loop, ported onto rt::AgentSession
# directly -- supersedes test_agent_session_tool_call_loop.cpp.
add_executable(test_rt_agent_session_tool_call_loop test_rt_agent_session_tool_call_loop.cpp)
target_link_libraries(test_rt_agent_session_tool_call_loop PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_tool_call_loop COMMAND test_rt_agent_session_tool_call_loop)

# ---- Regression proof for the resolve_codeact_ask()/ask-pending max_turns_ bypass fix, found by a
# red-team round dedicated to session_builder.hpp's own "finding 7" (docs/planning/quickstart-session-
# builder-design-draft.md). No real MediatedPythonRunner needed -- see this file's own top comment.
add_executable(test_rt_agent_session_codeact_ask_max_turns test_rt_agent_session_codeact_ask_max_turns.cpp)
target_link_libraries(test_rt_agent_session_codeact_ask_max_turns PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_codeact_ask_max_turns COMMAND test_rt_agent_session_codeact_ask_max_turns)

# ---- decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md wired for real:
# AgentSession::set_turn_middleware_hook() -- the turn/pre_model mechanism proven standalone in
# test_turn_middleware.cpp now proven against a real AgentSession round.
add_executable(test_rt_agent_session_turn_middleware test_rt_agent_session_turn_middleware.cpp)
target_link_libraries(test_rt_agent_session_turn_middleware PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_turn_middleware COMMAND test_rt_agent_session_turn_middleware)

# ---- decisions/ADR-069-content-triggered-model-response-replay.md wired for real: ContentReplayGateway
# plugs directly into AgentSession's existing ChatClientT slot (ModelCallGatewayLike), zero core changes.
add_executable(test_rt_agent_session_content_replay test_rt_agent_session_content_replay.cpp)
target_link_libraries(test_rt_agent_session_content_replay PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_content_replay COMMAND test_rt_agent_session_content_replay)

# ---- decisions/ADR-066-context-provider-attribution-provenance.md wired for real: a composed
# multi-provider HistoryProviderT slot (already-existing composition path, ComposedContextProvider)
# carries attribution all the way to a real AgentSession round's outbound ChatRequest.
add_executable(test_rt_agent_session_context_provenance test_rt_agent_session_context_provenance.cpp)
target_link_libraries(test_rt_agent_session_context_provenance PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_context_provenance COMMAND test_rt_agent_session_context_provenance)

# ---- decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md wired for real:
# QuarantineToolProvider (trust/secret_quarantine.hpp) occupies AgentSession's HistoryProviderT slot;
# a real tool-call round reaches the quarantine store through the actual invoke_tool() pipeline.
# Windows-only (QuarantineSecretStore depends on trust::hmac_sha256, agentengine::capability_token).
if(WIN32)
  add_executable(test_rt_agent_session_quarantine_tool test_rt_agent_session_quarantine_tool.cpp)
  target_link_libraries(test_rt_agent_session_quarantine_tool PRIVATE agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_rt_agent_session_quarantine_tool COMMAND test_rt_agent_session_quarantine_tool)
endif()

# ---- decisions/ADR-069-content-triggered-model-response-replay.md §7's own open question, answered
# with real evidence: ADR-067's turn/pre_model hook and ADR-069's ContentReplayGateway, both wired
# into ONE real AgentSession simultaneously -- proves they compose correctly, not merely independently.
add_executable(test_rt_agent_session_turn_and_replay_composition test_rt_agent_session_turn_and_replay_composition.cpp)
target_link_libraries(test_rt_agent_session_turn_and_replay_composition PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_turn_and_replay_composition COMMAND test_rt_agent_session_turn_and_replay_composition)

# ---- ADR-036 -- ModelCallGateway/MiddlewareModelCallGateway (retry+breaker+failover+middleware as
# a real coroutine, AgentSession-pluggable without a template-signature change).
add_executable(test_model_call_gateway test_model_call_gateway.cpp)
target_include_directories(test_model_call_gateway PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test_model_call_gateway PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_model_call_gateway COMMAND test_model_call_gateway)

# ---- ADR-037 Phase 2 -- porting G5 (test_model_call_gateway.cpp's own "AgentSession integration"
# block, the ONE case in that file that touched AgentSession/quark::TestKit at all) onto
# agentengine::rt::AgentSession directly. G1-G4/G6-G9 exercise ModelCallGateway/
# MiddlewareModelCallGateway standalone with no AgentSession involvement and are left untouched in
# test_model_call_gateway.cpp above.
add_executable(test_rt_model_call_gateway_session test_rt_model_call_gateway_session.cpp)
target_link_libraries(test_rt_model_call_gateway_session PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_model_call_gateway_session COMMAND test_rt_model_call_gateway_session)

# ---- ADR-037 Phase 1 -- agentengine::rt::task<T>, the first piece of AgentEngine's own runtime
# substrate (include/agentengine/rt/task.hpp), standalone and NOT yet wired into core/task.hpp.
add_executable(test_rt_task test_rt_task.cpp)
target_link_libraries(test_rt_task PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_task COMMAND test_rt_task)

# ---- ADR-037 Phase 1 -- agentengine::rt::channel<T> (include/agentengine/rt/channel.hpp), a bounded,
# thread-safe, backpressured producer/consumer channel meant to eventually replace
# quark::ReplyStream<T*> as core/stream.hpp's internal backend. Standalone, zero quark:: dependency,
# NOT yet wired into core/stream.hpp.
add_executable(test_rt_channel test_rt_channel.cpp)
target_link_libraries(test_rt_channel PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_channel COMMAND test_rt_channel)

# ---- ADR-037 Phase 1 -- agentengine::rt::SessionStore, the host-injected persistence interface
# (include/agentengine/rt/session_store.hpp) replacing quark::EventLog/FenceToken, standalone and NOT
# yet wired into core/agent_session.hpp's snapshot/checkpoint functions.
add_executable(test_rt_session_store test_rt_session_store.cpp)
target_link_libraries(test_rt_session_store PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_session_store COMMAND test_rt_session_store)

# ---- ADR-037 Phase 1 -- agentengine::rt::ThreadPool, the minimal executor for driving task<void> job
# completion off the calling thread (include/agentengine/rt/thread_pool.hpp), standalone and NOT yet
# wired into any live Quark-based code.
add_executable(test_rt_thread_pool test_rt_thread_pool.cpp)
target_link_libraries(test_rt_thread_pool PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_thread_pool COMMAND test_rt_thread_pool)

# ---- ADR-037 Phase 1 -- agentengine::rt::CircuitBreaker, a self-contained reimplementation of
# quark::CircuitBreaker's algorithm (include/agentengine/rt/circuit_breaker.hpp), standalone and NOT
# yet wired into core/model_call_gateway.hpp.
add_executable(test_rt_circuit_breaker test_rt_circuit_breaker.cpp)
target_link_libraries(test_rt_circuit_breaker PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_circuit_breaker COMMAND test_rt_circuit_breaker)

# ---- ADR-037 Phase 2 -- agentengine::rt::AsyncMutex, I1's local replacement now that AgentSession
# stops being a quark::Actor.
add_executable(test_rt_async_mutex test_rt_async_mutex.cpp)
target_link_libraries(test_rt_async_mutex PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_async_mutex COMMAND test_rt_async_mutex)

# ---- ADR-064 Design B -- agentengine::rt::drive_leaf_task<T>(), the narrow "leaf task" driver
# VectorRagContextProvider::recall's invoke uses (include/agentengine/rt/drive_leaf_task.hpp).
add_executable(test_rt_drive_leaf_task test_rt_drive_leaf_task.cpp)
target_link_libraries(test_rt_drive_leaf_task PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_drive_leaf_task COMMAND test_rt_drive_leaf_task)

# ---- ADR-037 Phase 2, Slice 1 -- agentengine::rt::AgentSession, the Quark-actor-free replacement
# for agentengine::AgentSession's core turn loop.
add_executable(test_rt_agent_session test_rt_agent_session.cpp)
target_link_libraries(test_rt_agent_session PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session COMMAND test_rt_agent_session)

# ---- Gap-16/21 fix (2026-08-14, decisions/ADR-042-*.md) -- ContextContribution::instructions
# actually reaching the model as a real role::system Message, taint-checked at the one explicit
# declassification site.
add_executable(test_rt_agent_session_instructions test_rt_agent_session_instructions.cpp)
target_link_libraries(test_rt_agent_session_instructions PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_instructions COMMAND test_rt_agent_session_instructions)

# ---- ADR-048 (gap-audit finding 19, Phase 1) -- fail-closed Media capability gate, pure function.
add_executable(test_outbound_media_capability_gate test_outbound_media_capability_gate.cpp)
target_link_libraries(test_outbound_media_capability_gate PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_outbound_media_capability_gate COMMAND test_outbound_media_capability_gate)

# ---- ADR-048 (gap-audit finding 19, Phase 1) -- the same gate, wired end-to-end into
# AgentSession::run_model_call(): a real run fails closed and chat() is never invoked.
add_executable(test_rt_agent_session_media_capability_gate test_rt_agent_session_media_capability_gate.cpp)
target_link_libraries(test_rt_agent_session_media_capability_gate PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_media_capability_gate COMMAND test_rt_agent_session_media_capability_gate)

# ---- ADR-049 (gap-audit finding 20 / 003 §8 Q2) -- cross-provider Reasoning exclusion, wired
# end-to-end into AgentSession::run_rounds().
add_executable(test_rt_agent_session_reasoning_provenance test_rt_agent_session_reasoning_provenance.cpp)
target_link_libraries(test_rt_agent_session_reasoning_provenance PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_reasoning_provenance COMMAND test_rt_agent_session_reasoning_provenance)

# ---- Gap-15 fix (2026-08-14, decisions/ADR-043-*.md) -- start_run_with_ack_policy()'s
# at_most_once/require_durable sequencing, and the turn-delta durable write/read round trip.
add_executable(test_rt_agent_session_ack_policy test_rt_agent_session_ack_policy.cpp)
target_link_libraries(test_rt_agent_session_ack_policy PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_ack_policy COMMAND test_rt_agent_session_ack_policy)

# ---- ADR-037 Phase 2, Slice 1 -- five real behavioral claims ported from the old, Quark-actor-based
# test_agent_session_state/test_agent_session_history_window/test_agent_session_fork/
# test_agent_session_redact/test_agent_session_interaction onto agentengine::rt::AgentSession
# directly (STATE/WIN/FORK/REDACT/INT) -- supersedes all five.
add_executable(test_rt_agent_session_lifecycle test_rt_agent_session_lifecycle.cpp)
target_link_libraries(test_rt_agent_session_lifecycle PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_lifecycle COMMAND test_rt_agent_session_lifecycle)

# ---- ADR-037 Phase 2, Slice 2 -- agentengine::rt::AgentSession's snapshot/checkpoint surface,
# built against rt::SessionStore instead of quark::FenceToken/snapshot_sequential.
add_executable(test_rt_agent_session_snapshot test_rt_agent_session_snapshot.cpp)
target_link_libraries(test_rt_agent_session_snapshot PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_snapshot COMMAND test_rt_agent_session_snapshot)

# ---- ADR-037 Phase 2, Slice 3 -- agentengine::rt::AgentSession's standing-effects/background-task
# surface, backed by a BackgroundCompletionQueue instead of quark::ActorRef::tell().
add_executable(test_rt_agent_session_background_task test_rt_agent_session_background_task.cpp)
target_link_libraries(test_rt_agent_session_background_task PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_background_task COMMAND test_rt_agent_session_background_task)

# ---- ADR-053 (gap-7 closure) -- agentengine::rt::AgentSession's THIRD real StandingEffect producer,
# schedule_wakeup()/due_standing_effects() (agent_session.hpp's "SLICE 4 ADDITION").
add_executable(test_rt_agent_session_schedule_wakeup test_rt_agent_session_schedule_wakeup.cpp)
target_link_libraries(test_rt_agent_session_schedule_wakeup PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_schedule_wakeup COMMAND test_rt_agent_session_schedule_wakeup)

# ---- ADR-053 §5 follow-up -- schedule_wakeup exposed as a real, model-callable declared tool.
add_executable(test_rt_agent_session_schedule_wakeup_tool test_rt_agent_session_schedule_wakeup_tool.cpp)
target_link_libraries(test_rt_agent_session_schedule_wakeup_tool PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_schedule_wakeup_tool COMMAND test_rt_agent_session_schedule_wakeup_tool)

# ---- ADR-037 Phase 3, Slice 1 -- agentengine::rt::WorkflowSupervisor, the Quark-actor-free
# replacement for workflow::WorkflowSupervisor's core superstep loop, fanning out through
# rt::ThreadPool instead of quark::ActorRef<FunctionExecutor>::ask<>.
add_executable(test_rt_workflow_supervisor test_rt_workflow_supervisor.cpp)
target_link_libraries(test_rt_workflow_supervisor PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_supervisor COMMAND test_rt_workflow_supervisor)

# ---- ADR-037 Phase 3, Slice 1 -- request-port behavior against rt::WorkflowSupervisor, superseding
# the old Quark-actor-based test_workflow_request_port(.cpp)/test_workflow_request_port_independent_qa
# (014 §4). E1/E2/E3/E5 and IQ1-IQ6, minus the two claims that are genuinely out of scope for `rt::`:
# E0 (shared, unchanged workflow/graph.hpp validator code) and the activation-passivation half of
# E1/E4 (there are no Quark activations in `rt::` for passivation to apply to).
add_executable(test_rt_workflow_supervisor_request_port test_rt_workflow_supervisor_request_port.cpp)
target_link_libraries(test_rt_workflow_supervisor_request_port PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_supervisor_request_port COMMAND test_rt_workflow_supervisor_request_port)

# ---- OQ-19: the runtime half of the agent-executor CapabilitySet bridge (rt/agent_workflow_
# executor.hpp) -- AgentExecutorBodyTag's positive control, WorkflowSupervisor::initialize()'s
# structural-marker refusal (closing test_workflow_graph_validation.cpp's own stale-comment gap),
# a real end-to-end dispatch through a scripted AgentSession, cyclic history accumulation within one
# run, the concurrent-same-node duplicate-delivery quarantine (both the default `fail` and a
# `propagate` failure policy), and the checkpoint/resume documented fresh-session limitation.
add_executable(test_rt_agent_workflow_executor test_rt_agent_workflow_executor.cpp)
target_link_libraries(test_rt_agent_workflow_executor PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_workflow_executor COMMAND test_rt_agent_workflow_executor)

# ---- ADR-037 Phase 3 -- 014 §7's live-view bullet (WorkflowSupervisor::enable_live_view) against
# rt::WorkflowSupervisor -- supersedes the old Quark-actor-based test_workflow_live_view.cpp (LV1/LV2,
# reused verbatim). One WorkflowLiveEvent per superstep boundary over a real agentengine::stream<T>,
# checked from the executor-state angle, plus the negative control (a round that fails the whole run
# produces no live event, proven by draining to empty, not just an unchecked assumption).
add_executable(test_rt_workflow_live_view test_rt_workflow_live_view.cpp)
target_link_libraries(test_rt_workflow_live_view PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_live_view COMMAND test_rt_workflow_live_view)

# ---- ADR-037 Phase 3, Slice 1 -- bound/failure-policy behavior (B3-B5, D1-D4) ported from the two
# retired Quark-actor-based test_workflow_superstep(.cpp)/test_workflow_failure(.cpp) onto
# rt::WorkflowSupervisor directly -- supersedes both.
add_executable(test_rt_workflow_supervisor_failure_policies test_rt_workflow_supervisor_failure_policies.cpp)
target_link_libraries(test_rt_workflow_supervisor_failure_policies PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_supervisor_failure_policies COMMAND test_rt_workflow_supervisor_failure_policies)

# ---- ADR-037 Phase 3 -- routing behavior (fan_in/switch_case/multi_selection/cyclic, plus retry
# composing with each) ported from the two retired Quark-actor-based
# test_workflow_patterns(.cpp)/test_workflow_patterns_resilience(.cpp) onto rt::WorkflowSupervisor
# directly -- supersedes both, including the I3 routing boundary check (RF-1/RF-2).
add_executable(test_rt_workflow_supervisor_patterns test_rt_workflow_supervisor_patterns.cpp)
target_link_libraries(test_rt_workflow_supervisor_patterns PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_supervisor_patterns COMMAND test_rt_workflow_supervisor_patterns)

# ---- ADR-051 (gap-audit finding 22 / 014 §8 G3) -- the 10^3-seed scheduling shuffle test, never
# built before this file: shuffled intra-round completion order produces identical output, and a
# deliberately order-dependent executor is caught by the same sweep.
add_executable(test_rt_workflow_supervisor_scheduling_shuffle test_rt_workflow_supervisor_scheduling_shuffle.cpp)
target_link_libraries(test_rt_workflow_supervisor_scheduling_shuffle PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_supervisor_scheduling_shuffle COMMAND test_rt_workflow_supervisor_scheduling_shuffle)

# ---- ADR-037 Phase 2, Slice 1 -- ADR-029's suspend-for-human-approval mechanism, re-proven against
# agentengine::rt::AgentSession -- supersedes the old, Quark-actor-based
# test_agent_session_suspend_approval(.cpp).
add_executable(test_rt_agent_session_suspend_approval test_rt_agent_session_suspend_approval.cpp)
target_link_libraries(test_rt_agent_session_suspend_approval PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_suspend_approval COMMAND test_rt_agent_session_suspend_approval)

# ---- OQ-21 (core/tool_call_hook.hpp) prove phase -- the tool-call hook stage wired into a real
# rt::AgentSession round: deny-before-approval, rewrite-reaches-dispatch-and-downgrades-provenance,
# a no-hook regression/positive control, and the interaction_reason::hook_decision suspend/resume
# path. Pure AgentSession/tool_pipeline logic, no CPython -- matches
# test_rt_agent_session_suspend_approval's own link set exactly.
add_executable(test_rt_agent_session_tool_call_hook test_rt_agent_session_tool_call_hook.cpp)
target_link_libraries(test_rt_agent_session_tool_call_hook PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_tool_call_hook COMMAND test_rt_agent_session_tool_call_hook)

# ---- ADR-044 prove phase -- EffectContext::report_progress(), a real call-scoped reverse channel
# into emit_run_event()/enable_event_stream() (run_event_kind::tool_call_delta), bracketed at all
# three real invoke_tool() call sites in rt/agent_session.hpp, plus a direct regression test against
# tool_pipeline.hpp::background_task() proving the §4 must-fix finding (a Backgroundable tool's
# invoke() must never reach a report_progress closure copied from a racing foreground bracket) is
# closed structurally. Pure AgentSession/tool_pipeline logic, no CPython -- belongs in the default
# `build` tree, matching test_rt_agent_session_suspend_approval's own link set exactly.
add_executable(test_agent_session_tool_call_progress test_agent_session_tool_call_progress.cpp)
target_link_libraries(test_agent_session_tool_call_progress PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_tool_call_progress COMMAND test_agent_session_tool_call_progress)

# ---- ADR-042 (Design B) prove phase -- OutputSchema<T> wired into a live rt::AgentSession turn:
# set_output_schema()'s additive opt-in, native-only request-side scoping, and response-side
# validation at the convergence branch. Pure AgentSession/ChatClient logic, no CPython -- belongs in
# the default `build` tree, matching test_rt_agent_session_suspend_approval's own link set exactly
# (agentengine::core + agentengine_warnings only, no AGENTENGINE_BUILD_PYTHON_RUNNER gate needed).
add_executable(test_agent_session_output_schema test_agent_session_output_schema.cpp)
target_link_libraries(test_agent_session_output_schema PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_session_output_schema COMMAND test_agent_session_output_schema)

# ---- ADR-030 -- session-scoped CodeAct wiring: the runner-binding claim/fail-closed primitive.
add_executable(test_codeact_runner_binding test_codeact_runner_binding.cpp)
target_link_libraries(test_codeact_runner_binding PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_codeact_runner_binding COMMAND test_codeact_runner_binding)

# ---- ADR-037 Phase 2 -- porting test_session_scoped_stateful_tools(.cpp) (ADR-028's session-scoped
# stateful tools mechanism), test_codeact_session_isolation(.cpp) (ADR-030's full-stack session
# isolation proof), and test_agent_session_delegation(.cpp) (018 §1's on_behalf_of delegation) onto
# agentengine::rt::AgentSession directly -- supersedes all three. test_codeact_runner_binding.cpp
# (above) is untouched -- it proves CodeActRunnerBinding<RunnerT>'s own claim/fail-closed logic in
# isolation, with no AgentSession involvement to migrate.
add_executable(test_rt_agent_session_tooling_and_delegation test_rt_agent_session_tooling_and_delegation.cpp)
target_link_libraries(test_rt_agent_session_tooling_and_delegation PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_tooling_and_delegation COMMAND test_rt_agent_session_tooling_and_delegation)

# ---- ADR-037 -- agentengine::rt::SpawnCostBudget, the Quark-actor-free replacement for
# 026 §9 Q1 / OQ-14's SpawnCostBudgetActor (rt::AsyncMutex replaces quark::Actor<Sequential> as the
# check-and-decrement serialization mechanism).
add_executable(test_rt_spawn_cost_budget test_rt_spawn_cost_budget.cpp)
target_link_libraries(test_rt_spawn_cost_budget PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_spawn_cost_budget COMMAND test_rt_spawn_cost_budget)

# ---- ADR-079 §7's TSan residual on SpawnPump (rt/agent_spawn.hpp) -- a portable, test-local
# reproduction of SpawnPump's own cost-consumption synchronization shape (rt::SpawnCostBudget only,
# no worktree/compute_digest dependency), so it builds and links on every platform including Linux,
# where the real SpawnPump (test_rt_agent_spawn.cpp, below, WIN32-only) cannot run under TSan.
add_executable(test_rt_spawn_pump_concurrency test_rt_spawn_pump_concurrency.cpp)
target_link_libraries(test_rt_spawn_pump_concurrency PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_spawn_pump_concurrency COMMAND test_rt_spawn_pump_concurrency)

# ---- ADR-037 -- agentengine::rt::ProjectRegistry, the Quark-actor-free replacement for the
# in-memory-index half (H1-H3) of 030 Sec6's ProjectRegistry (project/registry.hpp). H4/H5/G4 --
# project/project.hpp's own manifest snapshot and archived-member tail -- are NOT covered here (a
# separate, larger gap needing a real append-only log store primitive, named in
# include/agentengine/rt/project_registry.hpp's own banner) -- test_project_registry.cpp stays in
# place, not deleted, since it is not yet fully superseded.
add_executable(test_rt_project_registry test_rt_project_registry.cpp)
target_link_libraries(test_rt_project_registry PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_project_registry COMMAND test_rt_project_registry)

# ---- ADR-037 -- ProjectSupervisor's checkpoint-orchestration role (project/lifecycle.hpp),
# type-erased across heterogeneous rt::AgentSession<...> member instantiations without Quark's
# ActorRef<A>::passivate(). See project_supervisor.hpp's own banner for the "checkpoint, not
# passivate" scope narrowing.
add_executable(test_rt_project_supervisor test_rt_project_supervisor.cpp)
target_link_libraries(test_rt_project_supervisor PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_project_supervisor COMMAND test_rt_project_supervisor)

# ---- ADR-037 -- agentengine::rt::AppendLogStore, the append-only persistence primitive both
# rt::WorkflowSupervisor's own time-travel gap and rt::ProjectRegistry's own archived-tail gap name
# as their shared missing dependency. Not wired into either yet -- proven standalone first.
add_executable(test_rt_append_log_store test_rt_append_log_store.cpp)
target_link_libraries(test_rt_append_log_store PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_append_log_store COMMAND test_rt_append_log_store)

# ---- ADR-037 -- agentengine::rt::WorkflowSupervisor's time-travel surface (014 §5's "rewind to
# any retained checkpoint"), wiring rt::AppendLogStore into the gap workflow_supervisor.hpp's own
# Slice 2 banner named as deferred. Additive over Slice 2's existing single-slot
# save_workflow_checkpoint/load_workflow_checkpoint (rt::SessionStore, still used for ordinary
# crash-recovery resume) -- this file answers the genuinely different "rewind to ANY retained
# checkpoint" claim a single-slot store cannot serve.
add_executable(test_rt_workflow_time_travel test_rt_workflow_time_travel.cpp)
target_link_libraries(test_rt_workflow_time_travel PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_time_travel COMMAND test_rt_workflow_time_travel)

# ---- ADR-037 -- agentengine::rt::ProjectRegistry's archived-member tail (030 Sec6/§7), wiring
# rt::AppendLogStore into the gap project_registry.hpp's own banner named as deferred: "the archived
# tail is genuinely, structurally append-only... this needs a genuine append-only log store primitive
# nothing in this codebase has built yet." Ports the old test_project_registry.cpp's own H5 (exact
# archival order) and G4 (at-scale, 2000 members) claims functionally; test_project_registry.cpp
# stays in place, not deleted, since its H4/G4 manifest-snapshot-cost claims still have no rt::
# equivalent (no ProjectRecord manifest has been ported yet -- separate, smaller follow-up).
add_executable(test_rt_project_archive test_rt_project_archive.cpp)
target_link_libraries(test_rt_project_archive PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_project_archive COMMAND test_rt_project_archive)

# ---- ADR-037 -- agentengine::rt::ProjectRecord's manifest snapshot (rt::SessionStore, single-slot --
# active_members stays small by construction, so no AppendLogStore is needed here unlike the archived
# tail above) and the directed-lifecycle status-flip functions (pause_project/restore_project/
# archive_project), layered on rt::ProjectSupervisor's own checkpoint_members_and_workflows(). Closes
# the last named residual from both rt::ProjectRegistry's and rt::ProjectSupervisor's own banners.
add_executable(test_rt_project_manifest test_rt_project_manifest.cpp)
target_link_libraries(test_rt_project_manifest PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_project_manifest COMMAND test_rt_project_manifest)

# ---- ADR-037 -- 030 §7's own promotion gate G1 against rt::ProjectSupervisor/rt::pause_project --
# supersedes the old Quark-actor-based test_project_scale_isolation.cpp (and, together with
# test_rt_project_supervisor.cpp's own Q1/Q4, test_project_lifecycle.cpp too -- both retired, see
# their own git history). 100 real AgentSession objects, each with its own ProjectSupervisor and its
# own InMemorySessionStore; pausing #50 is proven to touch ONLY #50's own store (structural
# isolation, standing in for the old Dormant-census/latency measurements that have no rt:: analogue
# -- see this file's own header comment for why), while the other 99 each complete a real second Run
# uninterrupted.
add_executable(test_rt_project_scale_isolation test_rt_project_scale_isolation.cpp)
target_link_libraries(test_rt_project_scale_isolation PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_project_scale_isolation COMMAND test_rt_project_scale_isolation)

# ---- ADR-037 -- 014 §8's own promotion gate G2, reproven against agentengine::rt::WorkflowSupervisor
# -- supersedes the old Quark-actor-based test_workflow_checkpoint_g2.cpp. A 20-node chain, killed
# (simulated: a genuinely new instance restore_from_record()'d from nothing but the checkpoint) at
# EVERY one of its 20 superstep boundaries -- every resume must complete with output identical to an
# uninterrupted control. 21 plain host-held WorkflowSupervisor instances in sequence, never
# concurrent -- no Engine/Activation stack needed, so the old 300s timeout is dropped.
add_executable(test_rt_workflow_checkpoint_g2 test_rt_workflow_checkpoint_g2.cpp)
target_link_libraries(test_rt_workflow_checkpoint_g2 PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_workflow_checkpoint_g2 COMMAND test_rt_workflow_checkpoint_g2)

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

# ---- ADR-037 Phase 2, Slice 1 -- ports test_agent_session_admission.cpp (018 §2 admission),
# test_agent_session_isolation.cpp (001 §1 per-session isolation, session_actor_id() restated as a
# direct behavioral proof since rt:: has no actor-id concept), test_agent_session_poison_run.cpp
# (019 §4 PoisonRunPolicy<N> quarantine, copied dependency-free rather than pulled in from the
# Quark-coupled core/agent_session.hpp), and test_agent_session_run_identity.cpp (001 §1/§2 run_id/
# turn_index identity, extended with a within-run turn_index-advances proof) onto rt::AgentSession
# directly -- supersedes all four.
add_executable(test_rt_agent_session_identity_and_admission test_rt_agent_session_identity_and_admission.cpp)
target_link_libraries(test_rt_agent_session_identity_and_admission PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_identity_and_admission COMMAND test_rt_agent_session_identity_and_admission)

# ADR-061 §20-§29 (Tier 3 per-request authority mechanism) -- real positive/negative controls against
# the compiled build, not just design text.
add_executable(test_rt_agent_session_tier3_authority test_rt_agent_session_tier3_authority.cpp)
target_link_libraries(test_rt_agent_session_tier3_authority PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_tier3_authority COMMAND test_rt_agent_session_tier3_authority)

# v1 slice of docs/planning/dynamic-multi-agent-fanout-design-draft.md (rounds 1-14) --
# agentengine::rt::multi_agent::{Budget, spawn, spawn_with_retry, parallel}
# (include/agentengine/rt/multi_agent.hpp). Real positive/negative controls, including a genuine
# multi-threaded Budget contention test and a ThreadPool-backed parallel() fan-out, per this project's
# own "a real concurrency test under rt::ThreadPool, not a synchronous driver" bar.
add_executable(test_rt_multi_agent test_rt_multi_agent.cpp)
target_link_libraries(test_rt_multi_agent PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_multi_agent COMMAND test_rt_multi_agent)

# ---- ADR-037 Phase 2 -- porting test_agent_session_streaming_model_calls(.cpp) (ADR-034 streaming
# turn loop), test_agent_session_response_format_leak_scan(.cpp) (ADR-035 Phase 1 leak scan), and
# test_agent_session_run_event_stream(.cpp) (Milestone 7 Phase A run event stream) onto
# agentengine::rt::AgentSession directly -- supersedes all three.
add_executable(test_rt_agent_session_streaming_and_events test_rt_agent_session_streaming_and_events.cpp)
target_link_libraries(test_rt_agent_session_streaming_and_events PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agent_session_streaming_and_events COMMAND test_rt_agent_session_streaming_and_events)

# ---- Milestone 7 Phase C1 -- 011 MCP's JSON-RPC 2.0 wire envelope (protocol/mcp/json_rpc.hpp) ------
add_executable(test_mcp_json_rpc test_mcp_json_rpc.cpp)
target_link_libraries(test_mcp_json_rpc PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_json_rpc COMMAND test_mcp_json_rpc)

# ---- Milestone 7 Phase C2 -- 011 §4's MCP server role (protocol/mcp/server.hpp): server/discover,
# tools/list, tools/call over a real ToolTable and the real invoke_tool() pipeline -------------------
add_executable(test_mcp_server test_mcp_server.cpp)
target_link_libraries(test_mcp_server PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_server COMMAND test_mcp_server)

# ---- ADR-061 §39/§40 prove phase -- McpServer's per-request CapabilityGrant (decisions/ADR-061) -----
add_executable(test_mcp_capability_grant test_mcp_capability_grant.cpp)
target_link_libraries(test_mcp_capability_grant PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_capability_grant COMMAND test_mcp_capability_grant)

# ---- ADR-061 §46 prove phase -- ApprovalDecider widened to see the calling Principal, closing R27's
# `approve_` finding (decisions/ADR-061) ---------------------------------------------------------------
add_executable(test_approval_decider_principal test_approval_decider_principal.cpp)
target_link_libraries(test_approval_decider_principal PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_approval_decider_principal COMMAND test_approval_decider_principal)

# ---- Milestone 7 Phase C3 -- 011 §3's MCP client role (protocol/mcp/client.hpp): caching,
# pagination, digest-pinning/rug-pull detection, isError surfaced not thrown -------------------------
add_executable(test_mcp_client test_mcp_client.cpp)
target_link_libraries(test_mcp_client PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_client COMMAND test_mcp_client)

# ---- CodeAct tool-bridge union (core/codeact_tool_union.hpp, protocol/mcp/mcp_tool_bridge.hpp):
# agent's own tools + skill-unlocked tools + MCP-discovered tools merged into one bridge-ready
# ToolTable, cross-source name collisions rejected, MCP path proven against a real McpServer. No
# CPython dependency -- unconditional, unlike the mediated_python_runner-linked tests below.
add_executable(test_codeact_tool_union test_codeact_tool_union.cpp)
target_link_libraries(test_codeact_tool_union PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_codeact_tool_union COMMAND test_codeact_tool_union)

# ---- Milestone 7 Phase C4 -- the io.modelcontextprotocol/tasks extension (011 §3.6/§12): a
# Backgroundable tool call started via a real McpServer, polled to completion via tasks/get, cancelled
# via tasks/cancel -- reusing background_task() (real since Phase B) end to end -------------------
add_executable(test_mcp_tasks_extension test_mcp_tasks_extension.cpp)
target_link_libraries(test_mcp_tasks_extension PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_tasks_extension COMMAND test_mcp_tasks_extension)

# ADR-023 §7 R1/R2/R3: server-minted task handles are principal-bound, not-authorized is
# indistinguishable from not-found, and the A2A path really applies 018 §2 admission. Every negative
# case carries its own positive control (decisions/README.md's rule for security claims).
add_executable(test_task_principal_binding test_task_principal_binding.cpp)
target_link_libraries(test_task_principal_binding PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_task_principal_binding COMMAND test_task_principal_binding)

# ---- Milestone 7 Phase D1 -- 012 §0/§A.4's A2A v1.0 wire object model (protocol\a2a\types.hpp):
# Part/Message/Artifact/TaskStatus/Task round-trip, the removed `kind` discriminator, full
# SCREAMING_SNAKE enum serialization, and unknown-Part-kind forward compatibility -----------------
add_executable(test_a2a_types test_a2a_types.cpp)
target_link_libraries(test_a2a_types PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_a2a_types COMMAND test_a2a_types)

# ---- Milestone 7 Phase D2 -- 012 §2.1's Agent Card generation (protocol\a2a\agent_card.hpp): skills
# derived from a REAL register_agent<A>()-compiled AgentMetadata's ToolTable, never hand-maintained,
# advertising only what this codebase can actually serve (no fabricated bindings/capabilities) -------
add_executable(test_a2a_agent_card test_a2a_agent_card.cpp)
target_link_libraries(test_a2a_agent_card PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_a2a_agent_card COMMAND test_a2a_agent_card)

# ---- Milestone 7 Phase D3 -- 012 §1/§2.3's Task <- Run projection and Task management
# (protocol\a2a\mapping.hpp, protocol\a2a\server.hpp): ContentItem/Message <-> Part/Message mapping,
# and SendMessage/GetTask/CancelTask wired to a REAL agentengine::rt::AgentSession (test_a2a_server.cpp;
# ADR-037 later ported that wiring off quark::Engine/Actor/ActorRef onto rt::AgentSession) -----------
add_executable(test_a2a_mapping test_a2a_mapping.cpp)
target_link_libraries(test_a2a_mapping PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_a2a_mapping COMMAND test_a2a_mapping)

add_executable(test_a2a_server test_a2a_server.cpp)
target_link_libraries(test_a2a_server PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_a2a_server COMMAND test_a2a_server)

# ---- Milestone 7 Phase D4 -- 012 §3/§4a's A2A client role (protocol\a2a\client.hpp):
# send/get/cancel passthrough against a REAL A2aServer, plus digest-pinned Agent Card caching and
# rug-pull detection, mirroring McpClient's own identical discipline for MCP tool listings ----------
add_executable(test_a2a_client test_a2a_client.cpp)
target_link_libraries(test_a2a_client PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_a2a_client COMMAND test_a2a_client)

# ---- Milestone 7 Phase E1 -- 013 §2.1/§2.2's AG-UI event vocabulary (protocol\agui\types.hpp):
# every event kind's exact wire identifier and field shape, cited from docs/research/2026-a2a-and-
# agui-detail.md Part B -------------------------------------------------------------------------
add_executable(test_agui_types test_agui_types.cpp)
target_link_libraries(test_agui_types PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agui_types COMMAND test_agui_types)

# ---- ADR-037 Phase 2 -- 013 §2.1/§2.2's projection from the internal RunEvent stream onto AgUiEvent
# (protocol\agui\projection.hpp), ported onto rt::AgentSession directly for its two AgentSession-
# driven blocks (the hand-fed-RunEvent blocks needed no changes) -- supersedes test_agui_projection.
add_executable(test_rt_agui_projection test_rt_agui_projection.cpp)
target_link_libraries(test_rt_agui_projection PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_agui_projection COMMAND test_rt_agui_projection)

# ---- Milestone 7 Phase E3 -- 013 §4's SSE framing (protocol\agui\sse.hpp): one "data: <json>\n\n"
# frame per AG-UI event, proven against real RunEventProjector output ------------------------------
add_executable(test_agui_sse test_agui_sse.cpp)
target_link_libraries(test_agui_sse PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agui_sse COMMAND test_agui_sse)

# ---- Milestone 7 Phase E4 -- 012 §2.3/013 §3's A2A streaming projection (protocol\a2a\streaming.hpp)
# and 013 §3's MCP progress projection (protocol\mcp\progress.hpp), plus 013 §6 G3's cross-surface
# equivalence proof (a real run's SAME internal event sequence projected to both AG-UI and A2A) -----
add_executable(test_a2a_streaming test_a2a_streaming.cpp)
target_link_libraries(test_a2a_streaming PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_a2a_streaming COMMAND test_a2a_streaming)

add_executable(test_mcp_progress test_mcp_progress.cpp)
target_link_libraries(test_mcp_progress PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_mcp_progress COMMAND test_mcp_progress)

# ---- ADR-037 Phase 2 -- 013 §6 G3's cross-surface equivalence proof, ported onto rt::AgentSession
# directly for its AgentSession-driven setup -- supersedes test_cross_surface_equivalence.cpp.
add_executable(test_rt_cross_surface_equivalence test_rt_cross_surface_equivalence.cpp)
target_link_libraries(test_rt_cross_surface_equivalence PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_rt_cross_surface_equivalence COMMAND test_rt_cross_surface_equivalence)

# ---- Milestone 7 Phase F1 -- 015 §2/§3's YAML-subset parser (core\yaml_value.hpp): block/flow
# style, block literals, quoted/plain scalars -- proven against the RFC's own example documents ----
add_executable(test_yaml_value test_yaml_value.cpp)
target_link_libraries(test_yaml_value PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_yaml_value COMMAND test_yaml_value)

# ---- Milestone 7 Phase F2 -- 015 §3's Workflow-document compiler (workflow\yaml_compiler.hpp):
# compiles a parsed YAML Workflow document to graph.hpp's own Workflow, checked by the SAME
# validate_workflow() the C++ WorkflowBuilder form uses (I6) -----------------------------------
add_executable(test_workflow_yaml_compiler test_workflow_yaml_compiler.cpp)
target_link_libraries(test_workflow_yaml_compiler PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_workflow_yaml_compiler COMMAND test_workflow_yaml_compiler)

# ---- Milestone 7 Phase F3 -- 015 §2's Agent-document compiler (core\agent_yaml_compiler.hpp):
# compiles a parsed YAML Agent document to a real AgentMetadata, cross-checked against register_
# agent<A>()'s own C++-form output for the same agent (I6 in action) --------------------------
add_executable(test_agent_yaml_compiler test_agent_yaml_compiler.cpp)
target_link_libraries(test_agent_yaml_compiler PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_agent_yaml_compiler COMMAND test_agent_yaml_compiler)

# ---- ADR-054 (gap-4/gap-5 closure) -- ToolRegistry / ToolTable::from_names() (core/tool_registry.hpp).
add_executable(test_tool_registry test_tool_registry.cpp)
target_link_libraries(test_tool_registry PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_tool_registry COMMAND test_tool_registry)

# ---- Dependency-free JSON value/parser/serializer (M2 Phase B task B2, core/json_value.hpp) ----
# Header-only, core-tier -- links only agentengine::core (no nlohmann::json; this IS the seam that
# keeps nlohmann out of product code, see CONVENTIONS.md dependency-tier discipline).
add_executable(test_json_value test_json_value.cpp)
target_link_libraries(test_json_value PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_json_value COMMAND test_json_value)

# ---- wasmtime C API smoke proof (M2 Phase D task D1, decision 5) -------------------------------
# Only built when AGENTENGINE_WITH_WASM is explicitly turned ON (root CMakeLists.txt) -- proves the
# vendored dependency itself links and runs a real WebAssembly module, independent of the real
# SandboxBackend built further down (D3, decisions/ADR-010-*.md) and its tool-pipeline bridge
# (decisions/ADR-040-*.md) -- this smoke test predates both and is kept as the narrowest possible
# "does the dependency itself work" check.
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

  # ---- wasm tool-pipeline bridge proof (decisions/ADR-040-wasm-tool-pipeline-bridge.md) ----------
  # Wires WasmBackend-discovered tools into the real core/tool_pipeline.hpp ToolTable/invoke_tool --
  # same fixture/toolchain-availability posture as test_wasm_backend above (SKIP, not fail, when
  # cargo-component is unavailable); its own pure-logic wasm_tool_result_to_json() checks still run
  # regardless, see this test file's own header comment.
  add_executable(test_wasm_tool_bridge test_wasm_tool_bridge.cpp)
  target_link_libraries(test_wasm_tool_bridge PRIVATE agentengine::wasm_backend)
  target_compile_definitions(test_wasm_tool_bridge PRIVATE
    AE_WASM_FIXTURE_PATH="${AE_WASM_FIXTURE_WASM}")
  if(TARGET ae_wasm_fixture)
    add_dependencies(test_wasm_tool_bridge ae_wasm_fixture)
  endif()
  add_test(NAME test_wasm_tool_bridge COMMAND test_wasm_tool_bridge)
  set_tests_properties(test_wasm_tool_bridge PROPERTIES SKIP_RETURN_CODE 77)
  if(WIN32)
    set_tests_properties(test_wasm_tool_bridge PROPERTIES
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

# ---- Skills Phase 2: core/skill_tool_scoping.hpp -- pure ToolTable filtering plus the negative
# control proving invoke_tool actually rejects a real tool absent from the scoped table (not just
# cosmetic list-shortening). No skills/sandbox dependency -- core-tier only.
add_executable(test_tool_table_scoping test_tool_table_scoping.cpp)
target_link_libraries(test_tool_table_scoping PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_tool_table_scoping COMMAND test_tool_table_scoping)

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

# ---- SandboxBackendRegistry (docs/planning/sandbox-backend-registry-design-draft.md, Revision 2) --
# Closes check_sandbox_profile_availability()'s own "needs an Engine-level backend registry" gap.
# Header-only, core-tier. test_sandbox_backend_registry.cpp proves the registry itself (including the
# regression test for a real, confirmed Revision-1 per-call-default-construction bug);
# test_agent_registry_sandbox_backend_registry.cpp proves its wiring into register_agent<A>().
add_executable(test_sandbox_backend_registry test_sandbox_backend_registry.cpp)
target_link_libraries(test_sandbox_backend_registry PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_sandbox_backend_registry COMMAND test_sandbox_backend_registry)

add_executable(test_agent_registry_sandbox_backend_registry test_agent_registry_sandbox_backend_registry.cpp)
target_link_libraries(test_agent_registry_sandbox_backend_registry PRIVATE
  agentengine::core agentengine_warnings)
add_test(NAME test_agent_registry_sandbox_backend_registry COMMAND test_agent_registry_sandbox_backend_registry)

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
  #
  # Defined in its own directory scope (helpers/CMakeLists.txt) rather than here, because it must be
  # built WITHOUT sanitizer instrumentation and CMAKE_CXX_FLAGS is global. An instrumented child
  # imports the sanitizer runtime DLL from Program Files, which a zero-capability AppContainer has no
  # ACL grant for -- so it never reaches main and six containment tests fail while create() succeeds.
  # That subdirectory carries the full reasoning, including why granting the ACL would be the wrong
  # fix.
  add_subdirectory(helpers)

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

  # ---- ADR-004 SS11 item 4 decision spike: LPAC vs regular AppContainer -------------------------
  # Not a regression test (no ongoing "must pass" claim about LPAC itself) -- a one-time decision
  # experiment, kept in the tree (unlike the ADR's own original ac_setup.cpp/ac_run.cpp spike,
  # never committed) so the evidence is reproducible, not merely quoted in the ADR's prose.
  add_executable(adr004_lpac_decision_spike adr004_lpac_decision_spike.cpp)
  target_link_libraries(adr004_lpac_decision_spike PRIVATE
    agentengine::core agentengine::native_jail_backend agentengine_warnings)
  target_compile_definitions(adr004_lpac_decision_spike PRIVATE
    AE_HOSTILE_CHILD_EXE="$<TARGET_FILE:job_object_hostile_child>")
  add_dependencies(adr004_lpac_decision_spike job_object_hostile_child)
  add_test(NAME adr004_lpac_decision_spike COMMAND adr004_lpac_decision_spike)
  set_tests_properties(adr004_lpac_decision_spike PROPERTIES TIMEOUT 60)

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
    # RUN_SERIAL on every test in this Linux native_jail group (not just this one): each
    # LinuxNativeJailBackend defaults to the SAME host paths (delegated_cgroup_root_,
    # jail_root_base_) and mints ids from a process-local, not host-unique, counter starting at 0 --
    # two of these test BINARIES running concurrently (`ctest -j`) can independently compute the
    # identical "linux_native_jail-0" id and collide creating the same cgroup/jail_root directory.
    # Observed directly in this harness's own `ctest -j 4` run before this property existed.
    set_tests_properties(test_native_jail_backend_linux PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

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
    set_tests_properties(test_native_jail_abuse_corpus_linux PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)

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
    set_tests_properties(test_native_jail_parity_linux PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

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
    set_tests_properties(test_native_jail_ambient_authority_linux PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

    # ---- 008 §9 G2/G3 filesystem containment (pivot_root + bind-mount jail, Linux half) ------------
    # docs/planning/linux-native-jail-pivot-root-containment-design-draft.md's design + self-
    # red-team record, now built and proven: fs-escape (Windows abuse corpus's Case 4 analogue) plus
    # a read-only-bind-mount positive/negative pair (MUST-FIX 2).
    add_executable(test_native_jail_fs_containment_linux test_native_jail_fs_containment_linux.cpp)
    target_link_libraries(test_native_jail_fs_containment_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_fs_containment_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_fs_containment_linux hostile_child_posix)
    add_test(NAME test_native_jail_fs_containment_linux
      COMMAND bash -c "source ${CMAKE_CURRENT_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_fs_containment_linux>")
    set_tests_properties(test_native_jail_fs_containment_linux PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

    # ---- 008 §9 G4 teardown-cycle census (M2 Phase C task C6, Linux half) --------------------------
    add_executable(test_native_jail_teardown_cycles_linux test_native_jail_teardown_cycles_linux.cpp)
    target_link_libraries(test_native_jail_teardown_cycles_linux PRIVATE
      agentengine::core agentengine::linux_native_jail_backend agentengine_warnings)
    target_compile_definitions(test_native_jail_teardown_cycles_linux PRIVATE
      AE_HOSTILE_CHILD_POSIX_EXE="$<TARGET_FILE:hostile_child_posix>")
    add_dependencies(test_native_jail_teardown_cycles_linux hostile_child_posix)
    add_test(NAME test_native_jail_teardown_cycles_linux
      COMMAND bash -c "source ${CMAKE_CURRENT_SOURCE_DIR}/helpers/cgroup_v2_test_setup.sh && $<TARGET_FILE:test_native_jail_teardown_cycles_linux>")
    set_tests_properties(test_native_jail_teardown_cycles_linux PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
  endif()
endif()

# ---- KataBackend end-to-end test (src/backends/kata/) -- Linux only, requires a real Kata/-------
# containerd deployment (kata_backend.hpp's own header comment has the exact precondition). Gated
# behind its own flag, separate from AGENTENGINE_LINUX_SANDBOX_TESTS, since it needs a materially
# heavier deployment (containerd + a pinned Kata Containers release + KVM) than that flag's own
# namespace/cgroup-only precondition.
if(NOT WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND)
  option(AGENTENGINE_KATA_SANDBOX_TESTS
    "Build/run the KataBackend end-to-end test (needs a running containerd + Kata Containers deployment with a kata-clh runtime class already configured)"
    OFF)
  if(AGENTENGINE_KATA_SANDBOX_TESTS)
    add_executable(test_kata_backend_linux test_kata_backend_linux.cpp)
    target_link_libraries(test_kata_backend_linux PRIVATE
      agentengine::core agentengine::kata_backend agentengine_warnings)
    add_test(NAME test_kata_backend_linux COMMAND test_kata_backend_linux)
    # Real VM boot + several `ctr` round trips per case -- generous but bounded, matching this
    # suite's own Machine Safety discipline (CLAUDE.md) rather than an unbounded wait.
    set_tests_properties(test_kata_backend_linux PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

    # ---- Slice 2 (MountSpec/ResourceLimits/NetPolicy enforcement) end-to-end test ------------------
    add_executable(test_kata_backend_slice2_linux test_kata_backend_slice2_linux.cpp)
    target_link_libraries(test_kata_backend_slice2_linux PRIVATE
      agentengine::core agentengine::kata_backend agentengine_warnings)
    add_test(NAME test_kata_backend_slice2_linux COMMAND test_kata_backend_slice2_linux)
    # Several real VM boots (one per case, deliberately not reused across cases so each is an
    # independent create()) plus a deliberate 10s guest sleep under a 3s wall_ms cap -- generous but
    # bounded.
    set_tests_properties(test_kata_backend_slice2_linux PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
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

  # ---- ADR-021 prove phase -- the inbound bearer-token mechanism (trust/bearer_token.hpp),
  # closing the specific red-team findings named in that header/decisions/ADR-021 §6 ---------------
  add_executable(test_bearer_token_proof test_bearer_token_proof.cpp)
  target_link_libraries(test_bearer_token_proof PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_bearer_token_proof COMMAND test_bearer_token_proof)

  # ---- ADR-039 prove phase -- trust::principal_from_bearer_claims(), the canonical claims -> Principal
  # bridge every host-pluggable transport adapter is expected to call (decisions/ADR-039) -------------
  add_executable(test_principal_from_bearer_claims test_principal_from_bearer_claims.cpp)
  target_link_libraries(test_principal_from_bearer_claims PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_principal_from_bearer_claims COMMAND test_principal_from_bearer_claims)

  # ---- ADR-061 §31/§33/§34/§35 prove phase -- trust::EndpointId/EndpointRegistry/mint_endpoint_id()
  # and rt::request_authority_from_bearer_claims(), the bearer-credential-to-RequestAuthority bridge
  # for the Tier-3 mechanism §20-§30 already proved (decisions/ADR-061) -------------------------------
  add_executable(test_request_authority_bridge test_request_authority_bridge.cpp)
  target_link_libraries(test_request_authority_bridge PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_request_authority_bridge COMMAND test_request_authority_bridge)

  # ---- ADR-061 §42/§43 prove phase -- trust::steady_deadline_from() (shared clock-conversion
  # primitive) and mcp::capability_grant_from_bearer_claims(), the bearer-credential-to-CapabilityGrant
  # bridge for McpServer::dispatch()'s per-request grant (§39-§41, decisions/ADR-061) -----------------
  add_executable(test_capability_grant_bridge test_capability_grant_bridge.cpp)
  target_link_libraries(test_capability_grant_bridge PRIVATE
    agentengine::core agentengine::capability_token agentengine_warnings)
  add_test(NAME test_capability_grant_bridge COMMAND test_capability_grant_bridge)
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

# ---- Conflict evidence materialization (025 §4, gap-14 closure, ADR-055) ------------------------
if(WIN32)
  add_executable(test_worktree_conflict_evidence test_worktree_conflict_evidence.cpp)
  target_link_libraries(test_worktree_conflict_evidence PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_conflict_evidence COMMAND test_worktree_conflict_evidence)
endif()

# ---- shared-mode staleness note (025 §3/§10 Q2, Milestone 3 Phase B3) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto dependency as A1/B1/B2.
if(WIN32)
  add_executable(test_worktree_staleness test_worktree_staleness.cpp)
  target_link_libraries(test_worktree_staleness PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_worktree_staleness COMMAND test_worktree_staleness)
endif()

# ---- Workflow executor worktree scoping (014 §1 <-> 025 §3, ADR-032) ---------------------------
# Builds real Blob/Tree fixtures via put_blob/put_tree, same real-crypto (compute_digest)
# dependency as A1/B1/B2/B3's own tests above.
if(WIN32)
  add_executable(test_workflow_worktree_scoping test_workflow_worktree_scoping.cpp)
  target_link_libraries(test_workflow_worktree_scoping PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_workflow_worktree_scoping COMMAND test_workflow_worktree_scoping)
endif()

# ---- Merge-on-join wiring (025 §4, ADR-055 §6 follow-up) -- bridges worktree_scoping.hpp's
# make_merge_on_join_hook() with rt::WorkflowSupervisor's new MergeOnJoinHook for real, driving a
# branch-mode executor through the supervisor to completion for the first time in this codebase.
if(WIN32)
  add_executable(test_rt_workflow_supervisor_merge_on_join test_rt_workflow_supervisor_merge_on_join.cpp)
  target_link_libraries(test_rt_workflow_supervisor_merge_on_join PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_rt_workflow_supervisor_merge_on_join COMMAND test_rt_workflow_supervisor_merge_on_join)
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

# ---- Skills (009 §8b-8c): SkillsProvider mounts real skills through real worktree machinery ------
# Real Tree/Ref/Mount/mount_read, same real-crypto dependency as every other worktree test above.
if(WIN32)
  add_executable(test_skill_provider_mount test_skill_provider_mount.cpp)
  target_link_libraries(test_skill_provider_mount PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_skill_provider_mount COMMAND test_skill_provider_mount)
endif()

# ---- ADR-072: external, host-discovered skill sources (Claude Code / Codex / Copilot / generic
# agents installs already on the machine) -- real scratch-directory fixtures plus real evidence
# against this session's own already-installed tools, and a SkillsProvider integration proving the
# existing collision guard still protects discovered sources. WIN32-gated for the same reason
# test_skill_provider_mount is: it links agentengine::worktree_store for that integration proof.
if(WIN32)
  add_executable(test_external_skill_discovery test_external_skill_discovery.cpp)
  target_link_libraries(test_external_skill_discovery PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_external_skill_discovery COMMAND test_external_skill_discovery)
endif()

# ---- Skills Phase 3 (decisions/ADR-024's addendum): agent-triggered, on-demand skill mounting.
# Core-tier only (no native_jail/Python dependency) -- proves MountedSkillsState's own contract plus
# the load-bearing chain: invoke_tool genuinely rejects a skill-named tool before that skill is
# mounted, and the SAME call succeeds after, through the real SkillsProvider/scope_tools_to_mounted_
# skills/invoke_tool pipeline.
if(WIN32)
  add_executable(test_on_demand_skill_mount test_on_demand_skill_mount.cpp)
  target_link_libraries(test_on_demand_skill_mount PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_on_demand_skill_mount COMMAND test_on_demand_skill_mount)
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
  add_executable(test_tool_optimizer_provider test_tool_optimizer_provider.cpp)
  target_link_libraries(test_tool_optimizer_provider PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_tool_optimizer_provider COMMAND test_tool_optimizer_provider)
endif()

# ---- Skills Phase 2: src/backends/native_jail/skill_mount_materializer.hpp -- the bridge that was
# missing entirely before this session: SkillsProvider::mounted()'s Mounts, materialized to real files
# on a real host directory via materialize_mount, plus the reserved-mount-id collision guard. Needs
# agentengine::mediated_shell_runner for the real MediatedFileSystemAdapter (ADR-014), same as
# test_worktree_mount_sync.
if(WIN32)
  add_executable(test_skill_mount_materializer test_skill_mount_materializer.cpp)
  target_link_libraries(test_skill_mount_materializer PRIVATE
    agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store agentengine_warnings)
  add_test(NAME test_skill_mount_materializer COMMAND test_skill_mount_materializer)
  set_tests_properties(test_skill_mount_materializer PROPERTIES TIMEOUT 60)
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

  # ---- Jailed-Python-worker design (final spec, 2026-08-23): the minimum-slice PROVE-phase positive
  # controls (G-Q1/G-Q2/G-lifecycle-2) -- a runaway CPU loop and a memory bomb, run against the REAL
  # agentengine_python_worker.exe, are actually caught/killed by the new session-scoped watchdog +
  # Job Object cap, closing component-role-audit-tracker.md's Finding Q for real.
  add_executable(test_native_jail_python_worker_slice1 test_native_jail_python_worker_slice1.cpp)
  target_link_libraries(test_native_jail_python_worker_slice1 PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_native_jail_python_worker_slice1 PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_native_jail_python_worker_slice1 COMMAND test_native_jail_python_worker_slice1)
  set_tests_properties(test_native_jail_python_worker_slice1 PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- HandleRelay (ADR-081 Slice 2, decisions/ADR-085-jailed-python-worker-slice-2-handle-relay.md):
  # the two positive/negative controls the design draft's own §6 named but the pre-existing suite never
  # exercises -- a real successful socket relay round trip under a genuinely granted cap::NetOut, and a
  # kernel-level (not merely Python-wrapper-level) read-only enforcement check.
  add_executable(test_native_jail_python_worker_handle_relay test_native_jail_python_worker_handle_relay.cpp)
  target_link_libraries(test_native_jail_python_worker_handle_relay PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_native_jail_python_worker_handle_relay PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_native_jail_python_worker_handle_relay COMMAND test_native_jail_python_worker_handle_relay)
  set_tests_properties(test_native_jail_python_worker_handle_relay PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)

  # ---- 008 SS9 G7 (session boundary) -- decisions/ADR-082-native-jail-promotion-gate-008-9.md SS4:
  # a real, previously-untracked gap. Two jailed worker processes, live at once, prove a canary
  # planted in one is unreachable from the other -- the kernel-enforced OS-process boundary Slice 1
  # gave this claim, exercised for real for the first time.
  add_executable(test_native_jail_session_boundary_windows test_native_jail_session_boundary_windows.cpp)
  target_link_libraries(test_native_jail_session_boundary_windows PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_native_jail_session_boundary_windows PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_native_jail_session_boundary_windows COMMAND test_native_jail_session_boundary_windows)
  set_tests_properties(test_native_jail_session_boundary_windows PROPERTIES
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

  # ---- Skills Phase 2: a real mounted skill, materialized to a real host directory, read by real
  # Python code through a real mediated open() -- closes the gap where every prior skills test only
  # proved mount_read from test-harness C++, never from inside a running sandbox. Needs
  # agentengine::mediated_shell_runner too, for skill_mount_materializer.hpp's MediatedFileSystemAdapter
  # dependency (transitively brings in agentengine::worktree_store, relinked explicitly for clarity).
  add_executable(test_mediated_python_runner_skill_mounts test_mediated_python_runner_skill_mounts.cpp)
  target_link_libraries(test_mediated_python_runner_skill_mounts PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine::mediated_shell_runner
    agentengine::worktree_store agentengine_warnings)
  target_compile_definitions(test_mediated_python_runner_skill_mounts PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_mediated_python_runner_skill_mounts COMMAND test_mediated_python_runner_skill_mounts)
  set_tests_properties(test_mediated_python_runner_skill_mounts PROPERTIES
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

  # ---- ADR-041 (agent.ask suspend/replay, Design B) prove phase -----------------------------------
  # Needs a REAL rt::AgentSession<...> driving a REAL MediatedPythonRunner -- links both
  # agentengine::core (rt::AgentSession) and agentengine::mediated_python_runner, matching
  # test_mediated_python_runner_agent_tools' own link set plus the rt:: pieces
  # test_rt_agent_session_suspend_approval already needs.
  add_executable(test_agent_session_suspend_codeact_ask test_agent_session_suspend_codeact_ask.cpp)
  target_link_libraries(test_agent_session_suspend_codeact_ask PRIVATE
    agentengine::core agentengine::mediated_python_runner agentengine_warnings)
  target_compile_definitions(test_agent_session_suspend_codeact_ask PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}")
  add_test(NAME test_agent_session_suspend_codeact_ask COMMAND test_agent_session_suspend_codeact_ask)
  set_tests_properties(test_agent_session_suspend_codeact_ask PROPERTIES
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

  # ADR-002 §6 item 5 / backlog item #39: does Py_NewInterpreterFromConfig (PEP 684) actually work
  # as a per-session isolation unit on this concrete build, and does numpy load inside one? Talks
  # to CPython directly, same reason and same pattern as test_python_layer0_sweep above.
  add_executable(test_python_subinterpreter_spike test_python_subinterpreter_spike.cpp)
  target_include_directories(test_python_subinterpreter_spike PRIVATE "${AGENTENGINE_PYTHON_ROOT}/include")
  target_link_libraries(test_python_subinterpreter_spike PRIVATE
    agentengine::core "${AE_PYTHON_IMPORT_LIB}" agentengine_warnings)
  target_compile_definitions(test_python_subinterpreter_spike PRIVATE
    AE_PYTHON_HOME="${AGENTENGINE_PYTHON_HOME}"
    AE_PYTHON_SITE_PACKAGES="${AGENTENGINE_PYTHON_SITE_PACKAGES}")
  # TWO separate ctest entries, not one -- a real finding this investigation made (see the file's
  # own header comment): a failed numpy import inside a subinterpreter crashes the process on
  # teardown, so "strict" and "legacy" must never share a process, matching CLAUDE.md's "a test
  # proving a fork bomb is contained must not be able to take the machine with it".
  add_test(NAME test_python_subinterpreter_spike_strict COMMAND test_python_subinterpreter_spike strict)
  set_tests_properties(test_python_subinterpreter_spike_strict PROPERTIES
    ENVIRONMENT "${AE_PYTHON_TEST_ENV}" TIMEOUT 60)
  add_test(NAME test_python_subinterpreter_spike_legacy COMMAND test_python_subinterpreter_spike legacy)
  set_tests_properties(test_python_subinterpreter_spike_legacy PROPERTIES
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
