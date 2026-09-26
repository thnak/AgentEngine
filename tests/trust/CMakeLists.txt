# tests/trust/: capabilities, tokens, principals and secrets (include/agentengine/trust/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "trust" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- CapabilitySet enforcement mechanism (decisions/ADR-009-capability-set-enforcement-mechanism.md) --
# Header-only (trust/capability.hpp), core-tier -- links only agentengine::core.
agentengine_add_test(test_capability_enforcement LIBS agentengine::core agentengine_warnings)

# ---- Compile-time capability declaration tags (M2 Phase A task A2) -----------------------------
agentengine_add_test(test_capability_declaration_tags LIBS agentengine::core agentengine_warnings)

# ---- cap::NativeExec (decisions/ADR-071-native-unsandboxed-process-execution-providers.md) ------
# Header-only (trust/capability.hpp), core-tier -- links only agentengine::core.
agentengine_add_test(test_native_exec_capability LIBS agentengine::core agentengine_warnings)

# ---- CodeAct discoverability manifest (resolves OQ-16, OpenQuestions.md) -----------------------
# Header-only (trust/agent_library_manifest.hpp), core-tier -- links only agentengine::core.
agentengine_add_test(test_agent_library_manifest LIBS agentengine::core agentengine_warnings)

# ---- Milestone 5 Phase A (docs/planning/milestone-5-providers-identity-secrets-breakdown.md) ---
# SecretRef/SecretLease/SecretStore (trust/secret.hpp), a capability-gated wrapper over Quark's own
# SecretSource seam (020-Security §4) -- env/file resolution, scoping per name, rotation without
# restart, fail-closed without a granted cap::Secret.
agentengine_add_test(test_secret_store LIBS agentengine::core agentengine_warnings)

# decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md's prove phase.
# QuarantineSecretStore's content-addressed naming reuses trust::hmac_sha256 (trust/hmac.cpp/
# hmac_posix.cpp, ADR-021, Linux parity ADR-107) rather than a second, unaudited digest
# implementation -- that primitive now lives in the portable agentengine::hmac library
# (CMakeLists.txt's own "HMAC-SHA256 primitive -- Windows AND Linux" section), built on both
# platforms, so this test needs no platform gate any more (previously WIN32-only, when hmac_sha256
# was only reachable via the Windows-only agentengine::capability_token library).
agentengine_add_test(test_secret_quarantine LIBS agentengine::core agentengine::hmac agentengine_warnings)

# ---- decisions/ADR-107-hmac-sha256-linux-parity.md's own direct regression test -----------------
# A dedicated RFC 4231 known-answer test for trust::hmac_sha256()/constant_time_equal() -- prior
# coverage of this primitive was only INDIRECT (test_capability_token_proof/redteam.cpp,
# test_secret_quarantine.cpp, test_rt_agent_session_quarantine_tool.cpp above). Portable: only calls
# the platform-independent hmac.hpp interface, so it is wired unconditionally against
# agentengine::hmac (Windows: hmac.cpp/CNG-BCrypt; Linux: hmac_posix.cpp/RFC 2104-from-scratch) --
# the same source proves both backends agree with the real published standard.
agentengine_add_test(test_hmac_sha256 LIBS agentengine::core agentengine::hmac agentengine_warnings)

# ---- Milestone 5 Phase H1 -- Principal establishment/delegation/admission mechanism
# (trust/principal.hpp): make_embedded_principal/make_local_cli_principal/make_anonymous_principal,
# derive_on_behalf_of (007 §2, depth-bounded), principal_admitted_for (018 §2's shared ownership rule).
agentengine_add_test(test_principal_delegation LIBS agentengine::core agentengine_warnings)

# ---- Policy-reachability enumerator (M2 Phase F task F2, 007 §9 gate G6) -----------------------
# Proves include/agentengine/trust/policy_reachability.hpp's enumerator against
# ../tools/policy_reachability_fixture.hpp's reference set -- the same fixture
# tools/policy_reachability.cpp (the CI tool itself) runs, so the two can never drift apart.
# Header-only, core-tier.
add_executable(test_policy_reachability test_policy_reachability.cpp)
target_include_directories(test_policy_reachability PRIVATE "${CMAKE_SOURCE_DIR}/tools")
target_link_libraries(test_policy_reachability PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_policy_reachability COMMAND test_policy_reachability)

# ---- Capability bearer-token prove phase (decisions/ADR-005-capability-bearer-tokens-cross-process.md)
# Resolves OQ-3. Design A (capability_token.hpp) correctness + red-team, Design B
# (capability_registry.hpp) red-team, and the verify()-vs-check() cost comparison §5 rests on.
if(WIN32)
  agentengine_add_test(test_capability_token_proof
    LIBS agentengine::core agentengine::capability_token agentengine_warnings)

  agentengine_add_test(test_capability_token_redteam
    LIBS agentengine::core agentengine::capability_token agentengine_warnings)

  agentengine_add_test(test_capability_token_benchmark
    LIBS agentengine::core agentengine::capability_token agentengine_warnings)

  # ---- ADR-021 prove phase -- the inbound bearer-token mechanism (trust/bearer_token.hpp),
  # closing the specific red-team findings named in that header/decisions/ADR-021 §6 ---------------
  agentengine_add_test(test_bearer_token_proof
    LIBS agentengine::core agentengine::capability_token agentengine_warnings)

  # ---- ADR-039 prove phase -- trust::principal_from_bearer_claims(), the canonical claims -> Principal
  # bridge every host-pluggable transport adapter is expected to call (decisions/ADR-039) -------------
  agentengine_add_test(test_principal_from_bearer_claims
    LIBS agentengine::core agentengine::capability_token agentengine_warnings)

  # ---- ADR-061 §31/§33/§34/§35 prove phase -- trust::EndpointId/EndpointRegistry/mint_endpoint_id()
  # and rt::request_authority_from_bearer_claims(), the bearer-credential-to-RequestAuthority bridge
  # for the Tier-3 mechanism §20-§30 already proved (decisions/ADR-061) -------------------------------
  agentengine_add_test(test_request_authority_bridge
    LIBS agentengine::core agentengine::capability_token agentengine_warnings)
endif()

# ---- ADR-102 Phase 1 (decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md) ------
# IdentityAuthority/IdentityHandle/Grant<T> (trust/identity_authority.hpp) and AsyncQuota<T>
# (rt/async_quota.hpp) -- the identity-native sandbox/worktree design's (ADR-099) first primitives
# ported into production. Header-only; agentengine::core alone provides the include path.
agentengine_add_test(test_identity_authority_grant LIBS agentengine::core agentengine_warnings)
