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
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/quark/include"
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
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/third_party/quark/include"
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
