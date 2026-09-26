# Correctness gate (CONVENTIONS.md: "tests/ — correctness gate, one per load-bearing invariant").
# smoke_vocabulary proves the abstract-vocabulary headers under include/agentengine/ actually
# compile together and that the declared concepts (ChatClient, Runner, SandboxBackend) are
# satisfiable by a conforming type.

# Every test target: AgentSession-heavy test TUs exceed MSVC's default object-file section limit under ASan
# (C1128; PR #103 CI). /bigobj only changes the object format, so it is safe to apply to all of them.
if(MSVC)
  add_compile_options(/bigobj)
endif()

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

# ---- ADR-176 §14/§15 compile-fail proof -------------------------------------------------------
# A tool reply that declares `image` must carry `image_digest` and `image_digest_kind` as plain
# `std::string`s. The spelling this rejects -- `std::optional<std::string>` -- is invisible in the
# published reply schema (it emits the same `{"type":"string"}` fragment), so the wire-level gate in
# tests/support/image_provenance_shape.hpp cannot be the only thing standing here; a red-team round
# compiled exactly that reply and watched it ship an image reference with nothing beside it. Same two-file
# idiom as the proofs above: the negative must NOT compile, the positive MUST.
try_compile(AE_IMAGE_PROVENANCE_REJECTS_UNSTAMPABLE
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/image_provenance_unstampable"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/image_provenance_unstampable_reply.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_IMAGE_PROVENANCE_REJECTS_UNSTAMPABLE_LOG)
if(AE_IMAGE_PROVENANCE_REJECTS_UNSTAMPABLE)
  message(FATAL_ERROR
    "ADR-176 §14 VIOLATED: tests/compile_fail/image_provenance_unstampable_reply.cpp compiled, but a "
    "reply declaring `image` with optional/wrapped companions must be REJECTED -- it publishes three "
    "plain strings in its schema while defeating stamp_image_provenance(), so nothing else would catch "
    "it. Build log:\n${AE_IMAGE_PROVENANCE_REJECTS_UNSTAMPABLE_LOG}")
endif()

try_compile(AE_IMAGE_PROVENANCE_POSITIVE_CONTROL
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/image_provenance_stampable"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/image_provenance_stampable_reply.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_IMAGE_PROVENANCE_POSITIVE_CONTROL_LOG)
if(NOT AE_IMAGE_PROVENANCE_POSITIVE_CONTROL)
  message(FATAL_ERROR
    "ADR-176 §14 positive control FAILED: tests/compile_fail/image_provenance_stampable_reply.cpp must "
    "compile (three plain std::strings is what every real reply uses) but did not. Build log:\n"
    "${AE_IMAGE_PROVENANCE_POSITIVE_CONTROL_LOG}")
endif()
message(STATUS
  "ADR-176 §14 compile-fail proof: OK (an optional/wrapped provenance field is rejected; three plain "
  "std::strings compile)")

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

# ---- ADR-102 §3 C1 compile-fail proof (IdentityHandle/IdentityAuthority construction discipline) --
# Same try_compile() idiom as the CapabilitySet gate above: `IdentityHandle` has no public
# constructor (friend-gated to IdentityAuthority only), and `IdentityAuthority` itself is
# non-copyable (the one real singleton for this whole authority model). The main runtime test
# (test_identity_authority_grant.cpp) already exercises the legitimate mint_root()/adopt() paths for
# real, serving as this pair's own positive control -- no separate positive-control compile_fail file
# needed.
try_compile(AE_IDENTITY_HANDLE_REJECTS_DIRECT_CONSTRUCTION
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/identity_handle_no_direct_construction"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/identity_handle_no_direct_construction.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_IDENTITY_HANDLE_REJECTS_DIRECT_CONSTRUCTION_LOG)
if(AE_IDENTITY_HANDLE_REJECTS_DIRECT_CONSTRUCTION)
  message(FATAL_ERROR
    "ADR-102 §3 C1 VIOLATED: tests/compile_fail/identity_handle_no_direct_construction.cpp compiled, "
    "but IdentityHandle must have NO public constructor. "
    "Build log:\n${AE_IDENTITY_HANDLE_REJECTS_DIRECT_CONSTRUCTION_LOG}")
endif()

try_compile(AE_IDENTITY_AUTHORITY_REJECTS_COPY
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/identity_authority_no_copy"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/identity_authority_no_copy.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_IDENTITY_AUTHORITY_REJECTS_COPY_LOG)
if(AE_IDENTITY_AUTHORITY_REJECTS_COPY)
  message(FATAL_ERROR
    "ADR-102 §3 C1 VIOLATED: tests/compile_fail/identity_authority_no_copy.cpp compiled, but "
    "IdentityAuthority must be non-copyable (the one real singleton for this authority model). "
    "Build log:\n${AE_IDENTITY_AUTHORITY_REJECTS_COPY_LOG}")
endif()
message(STATUS
  "ADR-102 §3 C1 compile-fail proof: OK (IdentityHandle rejects direct construction; IdentityAuthority rejects copy)")

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

# ---- ADR-096 C2 compile-fail proof (SandboxToolProvider composed into a session makes
# AgentSession::fork_from() a compile error) -- ADR-102 Phase 5 residual -----------------------------
# `ComposedContextProvider<Ms...>`'s own copy-assignment operator is UNCONDITIONALLY `= delete`d
# (core/composed_context_provider.hpp:119-120, ADR-074) -- for ANY `Ms`, not specifically because
# `SandboxToolProvider` (src/backends/native_jail/sandbox_tool_provider.hpp, non-copyable by
# construction, holds `std::unique_ptr<SessionShellSandbox>`) happens to be non-copyable itself; this
# gate composes the REAL, production `SandboxToolProvider` (rather than a synthetic minimal stand-in)
# to exercise the exact composed type tools/sandboxed_shell_chat.cpp and
# tests/test_composed_sandbox_providers_live.cpp both use as a real session's `HistoryProviderT`. That
# unconditionally-deleted copy-assignment is load-bearing at `AgentSession::fork_from()`'s
# `history_provider_ = source.history_provider_;` statement, which must therefore fail to compile for
# that session type. ADR-096 §6 already recorded a claim shaped like this one "CORRECT, empirically
# proven" via a throwaway MSVC probe compiled once during that ADR's own red-team round and then
# discarded -- never preserved as a durable, checked-in regression guard. This pair closes that
# residual, same two-file idiom as every other tests/compile_fail/*.cpp pair in this file. It does NOT
# close (and does not claim to close) a separate residual found by this same gate's own 2026-08-28
# red-team round: `ComposedContextProvider<Ms...>`'s move-assignment is not deleted, so
# `target.history_provider() = std::move(source.history_provider());` still compiles -- see that
# class's own comment and ADR-102 §48. CLOSED at runtime, not by this compile-time gate, by ADR-116
# (2026-08-30): that statement no longer aliases live provider state across sessions -- `operator=`
# refuses it as a silent no-op, proven by tests/test_session_builder.cpp's own B20.
#
# Portable across Windows and Linux (2026-08-29, decisions/ADR-105-sandbox-tool-provider-composed-
# linux-parity.md) -- previously WIN32-gated on the same stale "SandboxToolProvider's own current
# platform scope" claim already corrected above; `agentengine_mediated_shell_runner`/
# `agentengine_worktree_store` have both had real Linux twins since ADR-103/ADR-014 Phase C4. Links
# the real `SandboxToolProvider` dependency chain directly as extra SOURCES -- same reason as the
# Milestone 3 A3 block above: try_compile() runs in an isolated mini-project with no visibility into
# this build's own ALIAS targets (agentengine::mediated_shell_runner, agentengine::worktree_store do
# not exist there), so the three .cpp files `agentengine_mediated_shell_runner` is built from
# (top-level CMakeLists.txt) plus the two `agentengine_worktree_store` is built from are compiled
# alongside the probe itself -- the Windows sources link `bcrypt` directly for `compute_digest`'s real
# Windows implementation, the Linux sources need no extra system library (the Linux `compute_digest`
# is a self-contained FIPS 180-4 implementation, `worktree_digest_posix.cpp`) -- deliberately, even
# for the probe that is EXPECTED to fail, so a permanently-missing link dependency can never
# masquerade as "correctly rejected."
if(WIN32)
  set(AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_DEPS
    "${PROJECT_SOURCE_DIR}/src/backends/native_jail/mediated_shell_parser.cpp"
    "${PROJECT_SOURCE_DIR}/src/backends/native_jail/mediated_shell_dispatch.cpp"
    "${PROJECT_SOURCE_DIR}/src/backends/native_jail/mediated_filesystem_adapter.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/worktree_digest.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/worktree_mount_fs.cpp"
    "${PROJECT_SOURCE_DIR}/src/rt/agent_session_core.cpp")
  set(AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_LINK_LIBRARIES bcrypt)
else()
  set(AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_DEPS
    "${PROJECT_SOURCE_DIR}/src/backends/native_jail/mediated_shell_parser.cpp"
    "${PROJECT_SOURCE_DIR}/src/backends/native_jail/mediated_shell_dispatch.cpp"
    "${PROJECT_SOURCE_DIR}/src/backends/native_jail/mediated_filesystem_adapter_posix.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/worktree_digest_posix.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/worktree_mount_fs_posix.cpp"
    "${PROJECT_SOURCE_DIR}/src/rt/agent_session_core.cpp")
  set(AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_LINK_LIBRARIES)
endif()

try_compile(AE_SANDBOX_TOOL_PROVIDER_REJECTS_FORK_FROM
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/sandbox_tool_provider_rejects_fork_from"
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/sandbox_tool_provider_rejects_fork_from.cpp"
    ${AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_DEPS}
  CMAKE_FLAGS
    "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/src"
    "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
  LINK_LIBRARIES ${AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_LINK_LIBRARIES}
  OUTPUT_VARIABLE AE_SANDBOX_TOOL_PROVIDER_REJECTS_FORK_FROM_LOG)
if(AE_SANDBOX_TOOL_PROVIDER_REJECTS_FORK_FROM)
  message(FATAL_ERROR
    "ADR-096 C2 VIOLATED: tests/compile_fail/sandbox_tool_provider_rejects_fork_from.cpp compiled, "
    "but composing SandboxToolProvider into a session's HistoryProviderT (via "
    "ComposedContextProvider<Ms...>) must make AgentSession::fork_from() fail to compile -- a fork "
    "must never alias one session's live sandbox filesystem view with another's. "
    "Build log:\n${AE_SANDBOX_TOOL_PROVIDER_REJECTS_FORK_FROM_LOG}")
endif()

try_compile(AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_POSITIVE_CONTROL
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/sandbox_tool_provider_fork_from_positive_control"
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/sandbox_tool_provider_fork_from_positive_control.cpp"
    ${AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_DEPS}
  CMAKE_FLAGS
    "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include;${PROJECT_SOURCE_DIR}/src"
    "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
  LINK_LIBRARIES ${AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_LINK_LIBRARIES}
  OUTPUT_VARIABLE AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_POSITIVE_CONTROL_LOG)
if(NOT AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_POSITIVE_CONTROL)
  message(FATAL_ERROR
    "ADR-096 C2 positive control FAILED: tests/compile_fail/"
    "sandbox_tool_provider_fork_from_positive_control.cpp must compile and link (the same Session "
    "type, minus the fork_from() call, is otherwise ordinary and legitimate) but did not. "
    "Build log:\n${AE_SANDBOX_TOOL_PROVIDER_FORK_FROM_POSITIVE_CONTROL_LOG}")
endif()
message(STATUS
  "ADR-096 C2 compile-fail proof: OK (composing SandboxToolProvider into a session makes "
  "fork_from() fail to compile; the same session type minus that call compiles and links cleanly)")

# ---- ADR-158 compile-fail proofs (tool concurrency-exclusivity policy, MUST-FIX 1 / §5) ---------
# `Tool<Derived, Policies...>` (core/tool.hpp) must reject, at compile time: (1) a tool declaring
# both `Parallelizable` and `ExclusivityGroup<Name>` -- two different, contradictory concurrency
# claims about the same tool (MUST-FIX 1); (2) a tool declaring more than one `ExclusivityGroup<
# Name>` -- ambiguous, and this project rejects outright rather than silently keeping only the
# last-declared one (§5's multi-declaration question). Header-only; no linking needed. No separate
# positive-control file, matching the ADR-102 §3 C1 gate's own precedent above: the real runtime
# test (tests/test_tool_exclusivity_group.cpp) already compiles and passes with bare `Parallelizable`
# alone, `ExclusivityGroup<Name>` alone, and two independent tools sharing one group name, serving as
# this pair's own positive control.
try_compile(AE_TOOL_REJECTS_PARALLELIZABLE_AND_EXCLUSIVITY_GROUP
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/tool_rejects_parallelizable_and_exclusivity_group"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/tool_rejects_parallelizable_and_exclusivity_group.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_TOOL_REJECTS_PARALLELIZABLE_AND_EXCLUSIVITY_GROUP_LOG)
if(AE_TOOL_REJECTS_PARALLELIZABLE_AND_EXCLUSIVITY_GROUP)
  message(FATAL_ERROR
    "ADR-158 MUST-FIX 1 VIOLATED: tests/compile_fail/"
    "tool_rejects_parallelizable_and_exclusivity_group.cpp compiled, but a tool must not be able to "
    "declare both Parallelizable and ExclusivityGroup<Name>. "
    "Build log:\n${AE_TOOL_REJECTS_PARALLELIZABLE_AND_EXCLUSIVITY_GROUP_LOG}")
endif()

try_compile(AE_TOOL_REJECTS_DUPLICATE_EXCLUSIVITY_GROUP
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/tool_rejects_duplicate_exclusivity_group"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/tool_rejects_duplicate_exclusivity_group.cpp"
  CXX_STANDARD 23
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE AE_TOOL_REJECTS_DUPLICATE_EXCLUSIVITY_GROUP_LOG)
if(AE_TOOL_REJECTS_DUPLICATE_EXCLUSIVITY_GROUP)
  message(FATAL_ERROR
    "ADR-158 §5 VIOLATED: tests/compile_fail/tool_rejects_duplicate_exclusivity_group.cpp compiled, "
    "but a tool must not be able to declare more than one ExclusivityGroup<Name>. "
    "Build log:\n${AE_TOOL_REJECTS_DUPLICATE_EXCLUSIVITY_GROUP_LOG}")
endif()
message(STATUS
  "ADR-158 compile-fail proof: OK (a tool declaring both Parallelizable and ExclusivityGroup<Name>, "
  "or more than one ExclusivityGroup<Name>, both fail to compile)")

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

# ---- ADR-195 §3.8 compile-fail proof (trial-running harness round-1 red-team fix) ---------------
# run_trial's own claim that it "refuses a client not wrapped in RecordingChatClient by type" was
# true of the wrapper TYPE but not of a caller passing an ALREADY-WRAPPED RecordingChatClient<X> as
# Inner -- a round-1 reviewer proved that compiled and chained the caller's own external sink onto
# every trial call. eval_trial.hpp now static_asserts against it; these two files prove the
# rejection is real and not over-broad (a plain Inner still compiles). The REJECTED file fails at
# the static_assert during compilation, before any linking is needed; the POSITIVE CONTROL fully
# instantiates run_trial, which reaches compute_digest() (worktree_digest{,_posix}.cpp) -- the same
# extra-sources/link-libraries shape ADR-096 C2's compile-fail proof above already uses.
if(WIN32)
  set(AE_EVAL_RUN_TRIAL_DEPS "${PROJECT_SOURCE_DIR}/src/core/worktree_digest.cpp"
                             "${PROJECT_SOURCE_DIR}/src/rt/agent_session_core.cpp")
  set(AE_EVAL_RUN_TRIAL_LINK_LIBRARIES bcrypt)
else()
  set(AE_EVAL_RUN_TRIAL_DEPS "${PROJECT_SOURCE_DIR}/src/core/worktree_digest_posix.cpp"
                             "${PROJECT_SOURCE_DIR}/src/rt/agent_session_core.cpp")
  set(AE_EVAL_RUN_TRIAL_LINK_LIBRARIES)
endif()

try_compile(AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_INNER
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/eval_run_trial_rejects_prewrapped_inner"
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/eval_run_trial_rejects_prewrapped_inner.cpp"
    ${AE_EVAL_RUN_TRIAL_DEPS}
  CMAKE_FLAGS
    "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
    "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
  LINK_LIBRARIES ${AE_EVAL_RUN_TRIAL_LINK_LIBRARIES}
  OUTPUT_VARIABLE AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_INNER_LOG)
if(AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_INNER)
  message(FATAL_ERROR
    "ADR-195 §3.8 VIOLATED: tests/compile_fail/eval_run_trial_rejects_prewrapped_inner.cpp compiled, "
    "but run_trial must reject an Inner that is already a RecordingChatClient<...> -- a pre-wrapped "
    "instance's own external sink would observe every trial call outside this trial's confinement. "
    "Build log:\n${AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_INNER_LOG}")
endif()

try_compile(AE_EVAL_RUN_TRIAL_PLAIN_INNER_POSITIVE_CONTROL
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/eval_run_trial_plain_inner_positive_control"
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/eval_run_trial_plain_inner_positive_control.cpp"
    ${AE_EVAL_RUN_TRIAL_DEPS}
  CMAKE_FLAGS
    "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
    "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
  LINK_LIBRARIES ${AE_EVAL_RUN_TRIAL_LINK_LIBRARIES}
  OUTPUT_VARIABLE AE_EVAL_RUN_TRIAL_PLAIN_INNER_POSITIVE_CONTROL_LOG)
if(NOT AE_EVAL_RUN_TRIAL_PLAIN_INNER_POSITIVE_CONTROL)
  message(FATAL_ERROR
    "ADR-195 §3.8 positive control FAILED: tests/compile_fail/"
    "eval_run_trial_plain_inner_positive_control.cpp must compile (a plain, unwrapped Inner is the "
    "intended, legitimate call shape) but did not. "
    "Build log:\n${AE_EVAL_RUN_TRIAL_PLAIN_INNER_POSITIVE_CONTROL_LOG}")
endif()
message(STATUS
  "ADR-195 §3.8 compile-fail proof: OK (run_trial rejects a pre-wrapped RecordingChatClient<...> as "
  "Inner; a plain Inner still compiles)")

# ---- ADR-195 §3.8 compile-fail proof, round 2: the SAME rejection for SummarizerT --------------
# Round 1's static_assert only checked Inner; a round-2 reviewer proved a pre-wrapped
# RecordingChatClient<X> passed DIRECTLY as SummarizerT compiled cleanly and let its external sink
# observe MemoryProvider::on_turn_end's per-turn content outside the trial's confinement -- the
# identical escape, on the parameter round 1 never looked at. This proves the round-2 fix is real;
# eval_run_trial_plain_inner_positive_control.cpp above (a plain Inner AND plain Summarizer) already
# serves as this fix's positive control -- no separate one needed.
try_compile(AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_SUMMARIZER
  "${CMAKE_CURRENT_BINARY_DIR}/compile_fail_checks/eval_run_trial_rejects_prewrapped_summarizer"
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/eval_run_trial_rejects_prewrapped_summarizer.cpp"
    ${AE_EVAL_RUN_TRIAL_DEPS}
  CMAKE_FLAGS
    "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
    "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
  LINK_LIBRARIES ${AE_EVAL_RUN_TRIAL_LINK_LIBRARIES}
  OUTPUT_VARIABLE AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_SUMMARIZER_LOG)
if(AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_SUMMARIZER)
  message(FATAL_ERROR
    "ADR-195 §3.8 VIOLATED: tests/compile_fail/eval_run_trial_rejects_prewrapped_summarizer.cpp "
    "compiled, but run_trial must reject a SummarizerT that is already a RecordingChatClient<...> -- "
    "a pre-wrapped instance's own external sink would observe every trial's per-turn summarizer "
    "traffic outside this trial's confinement. "
    "Build log:\n${AE_EVAL_RUN_TRIAL_REJECTS_PREWRAPPED_SUMMARIZER_LOG}")
endif()
message(STATUS
  "ADR-195 §3.8 compile-fail proof (round 2): OK (run_trial rejects a pre-wrapped "
  "RecordingChatClient<...> as SummarizerT too)")

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

# AGENTENGINE_FAST_TESTS: optimization off for every test target (cmake/AgentEngineFastBuild.cmake).
agentengine_fast_build_this_directory()

# ---- Test folders (issue #120 S11) -------------------------------------------------------------
# The tests live in subfolders that mirror include/agentengine/ and src/. Each folder has its own
# CMakeLists.txt, added below by agentengine_add_test_folder() (cmake/AgentEngineTests.cmake), which
# also gives every test in it the folder's name as a ctest label (`ctest -L rt/agent_session`) and
# keeps its working directory at this directory's binary dir, where it was before the split.
#
# Everything above that edits directory-scope flags (the NDEBUG strip, AGENTENGINE_FAST_TESTS,
# /bigobj) must stay above the add_subdirectory() calls: a subdirectory copies its parent's variables
# and directory properties when it is added, not later.
include(AgentEngineTests)
set(AE_TESTS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- nlohmann/json, a test-only dependency ------------------------------------------------------
# First used by test_recorded_chat_client (core/chat/). Declared here, before the folders are
# added, so any folder's tests can link nlohmann_json::nlohmann_json. It is test-only per
# CONVENTIONS.md's dependency-tier discipline: FetchContent-ed here (not in the root CMakeLists.txt)
# and linked only to test targets — never to agentengine_core or anything under
# include/agentengine/core.
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

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
endif()

# Every test executable is still written to <build>/tests/, as it was when this was one directory:
# CI, scripts and people run them from there. Set after add_subdirectory(helpers) above, whose
# hostile-child executables keep their own <build>/tests/helpers/ location.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

foreach(ae_test_folder IN ITEMS
  rt
  rt/project
  rt/agent_spawn
  rt/agent_session
  workflow
  memory
  eval
  core/rag
  core/json
  core/ledger
  core/chat
  core/context
  core/tools
  core/agent
  core/skills
  trust
  sandbox
  sandbox/execution_surface
  backends/kata
  backends/native_jail
  backends/native_process
  python
  tools
  plugin
  worktree
  protocol/mcp
  protocol/a2a
  protocol/agui
  protocol/openai
  protocol/anthropic
  testing)
  agentengine_add_test_folder(${ae_test_folder})
endforeach()

# ADR-182 §12 R11: engine code must not include agentengine/testing/.
add_test(NAME lint_testing_includes
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/check_testing_includes.cmake)

# ---- ADR-182 §16: one ctest per checked-in scenario, replayed with the scripted model (no network,
# no Claude). Scenarios are exported by the test driver (scenario_export), usually by a Claude tester
# sub-agent -- including from live-model runs, whose observed answers become the script.
file(GLOB AE_SCENARIO_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/scenarios/*.json)
foreach(scenario IN LISTS AE_SCENARIO_FILES)
  get_filename_component(scenario_name ${scenario} NAME_WE)
  add_test(NAME scenario_${scenario_name} COMMAND agentengine_scenario_runner
         --fixtures-root ${PROJECT_SOURCE_DIR}/tests/fixtures/test_driver ${scenario})
  set_tests_properties(scenario_${scenario_name} PROPERTIES LABELS "scenario" TIMEOUT 120)
endforeach()
