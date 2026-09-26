# tests/compile_fail/: compile-fail gates (issue #120 S9).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "compile_fail" ctest label for every test here (cmake/AgentEngineTests.cmake).
#
# Each gate is a pair. The NEGATIVE file must not compile: agentengine_compile_fail_test() makes it an
# EXCLUDE_FROM_ALL target plus the ctest compile_fail_<file>, which builds it and passes only if the
# compiler rejects it with the expected diagnostic. The POSITIVE CONTROL must compile and link:
# agentengine_compile_fail_control() makes it an ordinary executable in the ALL build, so the build
# fails if it stops compiling. Two files, not one: a fail-only suite can't distinguish "correctly
# rejected" from "nothing here compiles at all." cmake/AgentEngineCompileFail.cmake explains how a
# negative is kept from passing for the wrong reason (a missing file, a link error, a broken
# dependency), and why tests/'s directory flags (no NDEBUG, /Od under AGENTENGINE_FAST_TESTS) cannot
# change a verdict.
#
# These were configure-time try_compile() calls until #120 S9: slow (they ran serially on every fresh
# configure) and cached, so an existing build directory never re-checked them after a header change.
# Run them with `ctest -L compile_fail`.
include(AgentEngineCompileFail)

# ---- 007 §9 G2 compile-fail proof (docs/planning/milestone-1-core-substrate-breakdown.md task 2) --
# I3's static half: Tainted<T> must have NO implicit conversion to the untainted capability-API
# surface. tainted_declassify_positive_control.cpp proves the explicit unsafe_view() declassifier
# still compiles.
agentengine_compile_fail_control(compile_fail_tainted_declassify_positive_control
  SOURCE tainted_declassify_positive_control.cpp
  LIBS agentengine::core)
agentengine_compile_fail_test(tainted_no_implicit_conversion
  SOURCE tainted_no_implicit_conversion.cpp
  CONTROL compile_fail_tainted_declassify_positive_control
  # cl and gcc name Tainted<...> in the error; clang says "no matching function for call to
  # 'capability_shaped_api'" and names Tainted only in the note.
  EXPECT "Tainted<|capability_shaped_api"
  LIBS agentengine::core)

# ---- ADR-176 §14/§15 compile-fail proof -------------------------------------------------------
# A tool reply that declares `image` must carry `image_digest` and `image_digest_kind` as plain
# `std::string`s. The spelling this rejects -- `std::optional<std::string>` -- is invisible in the
# published reply schema (it emits the same `{"type":"string"}` fragment), so the wire-level gate in
# tests/support/image_provenance_shape.hpp cannot be the only thing standing here; a red-team round
# compiled exactly that reply and watched it ship an image reference with nothing beside it. Same two-file
# idiom as the proofs above: the negative must NOT compile, the positive MUST (three plain
# std::strings is what every real reply uses).
agentengine_compile_fail_control(compile_fail_image_provenance_stampable_reply
  SOURCE image_provenance_stampable_reply.cpp
  LIBS agentengine::core)
agentengine_compile_fail_test(image_provenance_unstampable_reply
  SOURCE image_provenance_unstampable_reply.cpp
  CONTROL compile_fail_image_provenance_stampable_reply
  EXPECT "must carry both companions as plain std::string"
  LIBS agentengine::core)

# ---- 007 §9 G1 / ADR-009 compile-fail proof (M2 Phase A task A4) -------------------------------
# Miniature "no ambient authority" claim, one level below A4's original full scope (a tool/agent
# declaration site doesn't exist yet -- that's Phase B/E): CapabilitySet itself has no construction
# path except grant_root(). The positive control proves grant_root(), the intended, legitimate
# construction path, compiles.
agentengine_compile_fail_control(compile_fail_capability_set_grant_root_positive_control
  SOURCE capability_set_grant_root_positive_control.cpp
  LIBS agentengine::core)
agentengine_compile_fail_test(capability_set_no_direct_construction
  SOURCE capability_set_no_direct_construction.cpp
  CONTROL compile_fail_capability_set_grant_root_positive_control
  EXPECT "CapabilitySet"
  LIBS agentengine::core)

# ---- ADR-102 §3 C1 compile-fail proof (IdentityHandle/IdentityAuthority construction discipline) --
# Same idiom as the CapabilitySet gate above: `IdentityHandle` has no public constructor
# (friend-gated to IdentityAuthority only), and `IdentityAuthority` itself is non-copyable (the one
# real singleton for this whole authority model). The main runtime test
# (test_identity_authority_grant.cpp) already exercises the legitimate mint_root()/adopt() paths for
# real, serving as this pair's own positive control -- no separate positive-control compile_fail file
# needed; it is the CONTROL the negatives' fixture builds first.
agentengine_compile_fail_test(identity_handle_no_direct_construction
  SOURCE identity_handle_no_direct_construction.cpp
  CONTROL test_identity_authority_grant
  EXPECT "IdentityHandle"
  LIBS agentengine::core)
agentengine_compile_fail_test(identity_authority_no_copy
  SOURCE identity_authority_no_copy.cpp
  CONTROL test_identity_authority_grant
  EXPECT "IdentityAuthority"
  LIBS agentengine::core)

# ---- ADR-012 compile-fail proof (SandboxProfile<P>'s template-parameter kind) -------------------
# `SandboxProfile<P>` (core/agent.hpp) constrains `P` via `SandboxProfileArg` (sandbox/sandbox.hpp) --
# a real `SandboxBackend`, or the `Strict` resolution selector. A non-conforming `P` (e.g. `int`) must
# fail to compile at the declaration site, not silently accept a nonsense type. The positive control
# proves Strict and a real conforming SandboxBackend are both legitimate SandboxProfile<P> arguments.
agentengine_compile_fail_control(compile_fail_sandbox_profile_positive_control
  SOURCE sandbox_profile_positive_control.cpp
  LIBS agentengine::core)
agentengine_compile_fail_test(sandbox_profile_rejects_non_conforming_type
  SOURCE sandbox_profile_rejects_non_conforming_type.cpp
  CONTROL compile_fail_sandbox_profile_positive_control
  EXPECT "SandboxProfile"
  LIBS agentengine::core)

# ---- 014 §1 compile-fail proof (Milestone 6 Phase A) -------------------------------------------
# "An edge that connects incompatible types fails to build -- at compile time for the C++ form."
# That claim is about a COMPILE failure, so it cannot be asserted from inside a test that must
# itself compile; a compile-fail gate is the only honest form it can take. Same two-file idiom as the
# gates above -- a fail-only check cannot distinguish "correctly rejected" from "this file never
# compiled for an unrelated reason", so the positive control connects a MATCHING pair (a
# type-compatible edge is the ordinary, intended case).
agentengine_compile_fail_control(compile_fail_workflow_edge_type_match_positive_control
  SOURCE workflow_edge_type_match_positive_control.cpp
  LIBS agentengine::core)
agentengine_compile_fail_test(workflow_edge_type_mismatch
  SOURCE workflow_edge_type_mismatch.cpp
  CONTROL compile_fail_workflow_edge_type_match_positive_control
  EXPECT "incompatible edge"
  LIBS agentengine::core)

# ---- Milestone 3 Phase A3 compile-fail proof (025 §2's Blob/Tree immutability) ------------------
# `InMemoryWorktreeObjectStore::put_blob`/`put_tree` (core/worktree.hpp) take ONLY the content --
# the digest is always DERIVED from it, never accepted as a caller-supplied parameter -- which is
# what makes "immutable bytes, addressed by digest" true by construction. A two-argument
# put_blob(digest, bytes)/put_tree(digest, tree) overload would reopen exactly that hole, so
# calling either shape must fail to compile. WIN32-gated, as it always was. All three link the real
# compute_digest implementation (agentengine::worktree_store) -- deliberately, even the two that are
# EXPECTED to fail. Without that, a negative control could stay "green" (fail to build) forever for
# the wrong reason -- a permanently missing link dependency -- and never actually re-test whether the
# forbidden overload exists once it did compile; this way, the only thing standing between "fails"
# and "builds clean" is the overload itself, matching the positive control's own build shape exactly.
# The positive control proves the real, one-argument put_blob/put_tree signatures compile and link.
if(WIN32)
  agentengine_compile_fail_control(compile_fail_worktree_object_store_positive_control
    SOURCE worktree_object_store_positive_control.cpp
    LIBS agentengine::worktree_store)
  agentengine_compile_fail_test(worktree_object_store_rejects_blob_digest_override
    SOURCE worktree_object_store_rejects_blob_digest_override.cpp
    CONTROL compile_fail_worktree_object_store_positive_control
    # MSVC names the function (C2660 'put_blob': function does not take 2 arguments); clang-cl names the parameter.
    EXPECT "put_blob|too many arguments to function call, expected single argument 'bytes'"
    LIBS agentengine::worktree_store)
  agentengine_compile_fail_test(worktree_object_store_rejects_tree_digest_override
    SOURCE worktree_object_store_rejects_tree_digest_override.cpp
    CONTROL compile_fail_worktree_object_store_positive_control
    # MSVC names the function (C2660 'put_tree': function does not take 2 arguments); clang-cl names the parameter.
    EXPECT "put_tree|too many arguments to function call, expected single argument 'tree'"
    LIBS agentengine::worktree_store)
endif()

# ---- ADR-096 C2 compile-fail proof (SandboxToolProvider composed into a session makes
# AgentSession::fork_from() a compile error) -- ADR-102 Phase 5 residual -----------------------------
# `ComposedContextProvider<Ms...>`'s own copy-assignment operator is UNCONDITIONALLY `= delete`d
# (core/composed_context_provider.hpp:119-120, ADR-074) -- for ANY `Ms`, not specifically because
# `SandboxToolProvider` (src/backends/native_jail/sandbox_tool_provider.hpp, non-copyable by
# construction, holds `std::unique_ptr<SessionShellSandbox>`) happens to be non-copyable itself; this
# gate composes the REAL, production `SandboxToolProvider` (rather than a synthetic minimal stand-in)
# to exercise the exact composed type tools/sandboxed_shell_chat.cpp and
# tests/sandbox/execution_surface/test_composed_sandbox_providers_live.cpp both use as a real session's `HistoryProviderT`. That
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
# refuses it as a silent no-op, proven by tests/core/context/test_session_builder.cpp's own B20.
#
# Portable across Windows and Linux (2026-08-29, decisions/ADR-105-sandbox-tool-provider-composed-
# linux-parity.md) -- previously WIN32-gated on the same stale "SandboxToolProvider's own current
# platform scope" claim already corrected above; `agentengine_mediated_shell_runner`/
# `agentengine_worktree_store` have both had real Linux twins since ADR-103/ADR-014 Phase C4. Both
# files link the real `SandboxToolProvider` dependency chain, agentengine::mediated_shell_runner,
# which brings agentengine::worktree_store and agentengine::core with it (the session core and the
# tool pipeline). The top-level CMakeLists.txt picks each library's platform sources: on Windows
# mediated_filesystem_adapter.cpp, worktree_digest.cpp (bcrypt) and worktree_mount_fs.cpp; on Linux
# their _posix twins, with no extra system library (the Linux `compute_digest` is a self-contained
# FIPS 180-4 implementation, `worktree_digest_posix.cpp`). Deliberately linked, even for the probe that
# is EXPECTED to fail, so a permanently-missing link dependency can never masquerade as "correctly
# rejected." The positive control proves the same Session type, minus the fork_from() call, is
# otherwise ordinary and legitimate: it compiles and links.
agentengine_compile_fail_control(compile_fail_sandbox_tool_provider_fork_from_positive_control
  SOURCE sandbox_tool_provider_fork_from_positive_control.cpp
  LIBS agentengine::mediated_shell_runner)
agentengine_compile_fail_test(sandbox_tool_provider_rejects_fork_from
  SOURCE sandbox_tool_provider_rejects_fork_from.cpp
  CONTROL compile_fail_sandbox_tool_provider_fork_from_positive_control
  # cl and gcc name the deleted ComposedContextProvider<...>::operator=; clang may say only
  # "overload resolution selected deleted operator '='".
  EXPECT "ComposedContextProvider|deleted operator '='"
  LIBS agentengine::mediated_shell_runner)

# ---- ADR-158 compile-fail proofs (tool concurrency-exclusivity policy, MUST-FIX 1 / §5) ---------
# `Tool<Derived, Policies...>` (core/tool.hpp) must reject, at compile time: (1) a tool declaring
# both `Parallelizable` and `ExclusivityGroup<Name>` -- two different, contradictory concurrency
# claims about the same tool (MUST-FIX 1); (2) a tool declaring more than one `ExclusivityGroup<
# Name>` -- ambiguous, and this project rejects outright rather than silently keeping only the
# last-declared one (§5's multi-declaration question). Header-only. No separate positive-control
# file, matching the ADR-102 §3 C1 gate's own precedent above: the real runtime test
# (tests/core/tools/test_tool_exclusivity_group.cpp) already compiles and passes with bare
# `Parallelizable` alone, `ExclusivityGroup<Name>` alone, and two independent tools sharing one group
# name, serving as this pair's own positive control (the CONTROL below).
agentengine_compile_fail_test(tool_rejects_parallelizable_and_exclusivity_group
  SOURCE tool_rejects_parallelizable_and_exclusivity_group.cpp
  CONTROL test_tool_exclusivity_group
  EXPECT "never both"
  LIBS agentengine::core)
agentengine_compile_fail_test(tool_rejects_duplicate_exclusivity_group
  SOURCE tool_rejects_duplicate_exclusivity_group.cpp
  CONTROL test_tool_exclusivity_group
  EXPECT "at most one ExclusivityGroup"
  LIBS agentengine::core)

# ---- ADR-071 compile-fail proof (native-execution family distinctness) -------------------------
# `NativeCapabilityAnnouncer<Ps...>` (src/backends/native_process/native_capability_announcer.hpp)
# must reject composing two providers sharing the same declared family name (e.g. two
# `NativePythonProvider`s) -- otherwise an LLM could be handed two functionally-identical
# native-execution tools with no principled way to choose between them. Header-only: the `requires`
# clause is checked at the point the type alias itself is named, before any member function is
# instantiated, so both files need only the src/ include path and agentengine::core, not
# agentengine::native_process_backend. WIN32-gated: this backend's headers are Windows-only today
# (native_providers.hpp includes <windows.h> unconditionally), matching AGENTENGINE_WITH_NATIVE_
# PROCESS's own current platform scope -- and independent of whether that CMake option happens to be
# ON for this particular configure, since neither file links the backend library the option builds.
# The positive control proves four DISTINCT families composing together, the intended, legitimate
# path, compiles.
if(WIN32)
  agentengine_compile_fail_control(compile_fail_native_capability_announcer_distinct_families_positive_control
    SOURCE native_capability_announcer_distinct_families_positive_control.cpp
    LIBS agentengine::core
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src")
  agentengine_compile_fail_test(native_capability_announcer_rejects_duplicate_family
    SOURCE native_capability_announcer_rejects_duplicate_family.cpp
    CONTROL compile_fail_native_capability_announcer_distinct_families_positive_control
    EXPECT "NativeCapabilityAnnouncer"
    LIBS agentengine::core
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src")
endif()

# ---- ADR-195 §3.8 compile-fail proof (trial-running harness round-1 red-team fix) ---------------
# run_trial's own claim that it "refuses a client not wrapped in RecordingChatClient by type" was
# true of the wrapper TYPE but not of a caller passing an ALREADY-WRAPPED RecordingChatClient<X> as
# Inner -- a round-1 reviewer proved that compiled and chained the caller's own external sink onto
# every trial call. eval_trial.hpp now static_asserts against it; these two files prove the
# rejection is real and not over-broad (a plain, unwrapped Inner, the intended call shape, still
# compiles). The REJECTED file fails at the static_assert during compilation, before any linking is
# needed; the POSITIVE CONTROL fully instantiates run_trial, which reaches compute_digest()
# (worktree_digest{,_posix}.cpp), so both link agentengine::worktree_store (and agentengine::core with
# it) -- the same real-library shape ADR-096 C2's compile-fail proof above uses, and for the same
# reason: a permanently-missing link dependency must never read as "correctly rejected".
agentengine_compile_fail_control(compile_fail_eval_run_trial_plain_inner_positive_control
  SOURCE eval_run_trial_plain_inner_positive_control.cpp
  LIBS agentengine::worktree_store)
agentengine_compile_fail_test(eval_run_trial_rejects_prewrapped_inner
  SOURCE eval_run_trial_rejects_prewrapped_inner.cpp
  CONTROL compile_fail_eval_run_trial_plain_inner_positive_control
  EXPECT "Inner must not already be a RecordingChatClient"
  LIBS agentengine::worktree_store)

# ---- ADR-195 §3.8 compile-fail proof, round 2: the SAME rejection for SummarizerT --------------
# Round 1's static_assert only checked Inner; a round-2 reviewer proved a pre-wrapped
# RecordingChatClient<X> passed DIRECTLY as SummarizerT compiled cleanly and let its external sink
# observe MemoryProvider::on_turn_end's per-turn content outside the trial's confinement -- the
# identical escape, on the parameter round 1 never looked at. This proves the round-2 fix is real;
# eval_run_trial_plain_inner_positive_control.cpp above (a plain Inner AND plain Summarizer) already
# serves as this fix's positive control -- no separate one needed.
agentengine_compile_fail_test(eval_run_trial_rejects_prewrapped_summarizer
  SOURCE eval_run_trial_rejects_prewrapped_summarizer.cpp
  CONTROL compile_fail_eval_run_trial_plain_inner_positive_control
  EXPECT "SummarizerT must not already be a RecordingChatClient"
  LIBS agentengine::worktree_store)
