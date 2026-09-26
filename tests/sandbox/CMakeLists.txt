# tests/sandbox/: sandbox backends contract, egress, mandatory sandbox provider and task branches (include/agentengine/sandbox/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "sandbox" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- Egress proxy (M2 Phase F task F1, decisions/ADR-011-first-party-egress-proxy.md) ----------
# Header-only claims (is_blocked_address, reject_crlf) plus real loopback-socket claims via a
# TestHttpServer double, agentengine::net_egress_proxy (src/sandbox/net_egress_proxy.cpp).
agentengine_add_test(test_net_egress_proxy LIBS agentengine::net_egress_proxy agentengine_warnings)

# ---- HTTPS egress TLS client proof (decisions/ADR-013-https-egress-tls-client.md) --------------
# Only built when AGENTENGINE_WITH_HTTPS is explicitly turned ON (root CMakeLists.txt) -- links
# directly against mbedTLS (test-only in-memory certificate generation, a loopback TLS test server)
# in addition to agentengine::tls_client itself.
if(AGENTENGINE_WITH_HTTPS)
  agentengine_add_test(test_https_egress
    LIBS agentengine::tls_client agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509
      MbedTLS::mbedcrypto)

  # ---- Milestone 5 Phase C: host-initiated provider HTTP client -------------------------------
  agentengine_add_test(test_provider_http_client
    LIBS agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509
      MbedTLS::mbedcrypto)

  # ---- ADR-016 gates G1-G4: host-initiated provider egress address policy + transport --------------
  # The positive control for ADR-016. It relaxes the SSRF blocked-range table on the provider path
  # while leaving it enforced on the WASM guest path -- only defensible if BOTH halves are shown
  # against the SAME live loopback address in the SAME run, which is what this does. Deterministic and
  # offline (a plain-HTTP loopback server, no credential, no egress), so it is a DEFAULT-suite test,
  # deliberately not labelled live-network. ADR-016's G5 (the motivating case really works) is
  # test_llamacpp_live_e2e.cpp's.
  agentengine_add_test(test_provider_egress_address_policy
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

# ---- ADR-170 (GitHub issue #64) -- the PRODUCER side of run_event_kind::sandbox_exec_started/
# finished, which 013 §1 declares and protocol/agui/projection.hpp already projects, but which
# nothing in the tree ever emitted. S1-S9: the SandboxExecScope bracket and its fail-closed default,
# end-to-end emission through a real AgentSession event stream, the silent-tool positive control, the
# per-call bracket, background_task()'s detached-thread reset, the AG-UI projection of the new
# payload fields, and a real shipped producer (SessionShellSandbox's run_shell) through the real
# 006 §3 pipeline. Links mediated_shell_runner/worktree_store for S9, like test_session_shell_wiring.
agentengine_add_test(test_sandbox_exec_events
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings)

# ---- SandboxProfile<P>'s template-parameter kind (decisions/ADR-012-sandbox-profile-template-parameter-kind.md) --
# Header-only (core/agent_registry.hpp, sandbox/sandbox.hpp), core-tier.
agentengine_add_test(test_sandbox_profile_kind LIBS agentengine::core agentengine_warnings)

# ---- SandboxBackend contract completion (M2 Phase C task C1, 008 §2-3) -------------------------
# ProfileTraits / resolve_strict (Profile::Strict resolution) and MountSpec's host-path/blob-store
# source field. Header-only, core-tier -- links only agentengine::core.
agentengine_add_test(test_sandbox_backend_contract LIBS agentengine::core agentengine_warnings)

# ---- authorize_spec() (docs/planning/sandbox-spec-capability-enforcement-design-draft.md, ADR-087) -
# Pure-logic coverage of the capability<->mounts/net enforcement mechanism every mount/net-shaped
# SandboxBackend (KataBackend, LinuxNativeJailBackend, NativeJailBackend) now calls first. No OS
# dependency -- runs on every platform, unlike the Linux/Windows-specific backend suites that prove
# the real end-to-end wiring.
agentengine_add_test(test_sandbox_capability_authorization LIBS agentengine::core agentengine_warnings)

# ---- SandboxBackendRegistry (docs/planning/sandbox-backend-registry-design-draft.md, Revision 2) --
# Closes check_sandbox_profile_availability()'s own "needs an Engine-level backend registry" gap.
# Header-only, core-tier. test_sandbox_backend_registry.cpp proves the registry itself (including the
# regression test for a real, confirmed Revision-1 per-call-default-construction bug);
# test_agent_registry_sandbox_backend_registry.cpp proves its wiring into register_agent<A>().
agentengine_add_test(test_sandbox_backend_registry LIBS agentengine::core agentengine_warnings)

# ---- build_default_sandbox_registry() (docs/planning/default-sandbox-registry-wiring-design-draft.md) --
# Proves the real, compiled-in SandboxBackend conformers register with the right names/eligibility --
# unlike the two tests above, which prove the registry MECHANISM against fake conformers, this one
# exercises the real NativeJailBackend/LinuxNativeJailBackend, and WasmBackend/KataBackend whenever
# this build was actually configured with AGENTENGINE_WITH_WASM/AGENTENGINE_BUILD_KATA_BACKEND.
agentengine_add_test(test_default_sandbox_registry
  LIBS agentengine::default_sandbox_registry agentengine_warnings)
if(AGENTENGINE_WITH_WASM AND WIN32)
  # Windows has no rpath; wasmtime.dll must be reachable via PATH at run time -- same wiring
  # test_wasmtime_smoke already needs (tests/CMakeLists.txt's own AGENTENGINE_WITH_WASM block).
  set_tests_properties(test_default_sandbox_registry PROPERTIES
    ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")
endif()

# ---- ExecState / SessionExecStateRegistry (010 §3a, Milestone 3 Phase E1) -----------------------
# Header-only (sandbox/runner.hpp) -- no platform-specific dependency, proven with mock Runners
# since PythonRunner/ShellRunner (Phase E2/E3) don't exist yet.
agentengine_add_test(test_exec_state LIBS agentengine::core agentengine_warnings)

# ---- ADR-096 Design B: SandboxToolProvider (backends/native_jail/sandbox_tool_provider.hpp) --
# -- a ContextProvider composed via ComposedContextProvider<Ms...>, giving Shell a real,
# reusable, per-session lifecycle. Proven end to end: composition/concept conformance, lazy
# sandbox construction + ctx.sandbox_fs population + run_shell contribution on on_context(),
# reuse across a later round, the real invoke_tool() pipeline, and the digest-subdirectory
# defense-in-depth check (C8).
agentengine_add_test(test_sandbox_tool_provider
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings)

# ---- ADR-102 Phase 3 -- ExecutionSurface/SandboxRuntime/DockerExecutionSurface/RealIoFileSystem ---
# (sandbox/{execution_surface,docker_execution_surface,real_io_filesystem,sandbox_runtime}.hpp).
# Needs agentengine::worktree_store the same way test_ledger does (real Ledger/compute_digest), plus
# RealIoFileSystem's own real open_within_mount_root/WriteFile/ReadFile use, from the same target.
# REQUIRES a running Docker daemon reachable via `docker` on PATH -- every check shells out to a real
# container, matching this phase's own "never a mock" discipline; not run in an environment without one.
agentengine_add_test(test_sandbox_runtime
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)
set_tests_properties(test_sandbox_runtime PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- ADR-174 (GitHub issue #68) -- the root-owned ustar seed archive, OFFLINE half.
# Needs no daemon, no network and no privileges: it asserts the ARCHIVE BYTES (uid/gid 0, the POSIX
# ustar magic, the reader-recomputed checksum, block padding and terminators, the symlink and cap
# refusals) plus the regression guards for the defects two adversarial review rounds found -- ".."
# checked per-component rather than as a substring, long names carried by PAX instead of refused, and
# non-ASCII names reaching the header as UTF-8 rather than through the Windows active code page.
# Registered unconditionally and deliberately kept OUT of the daemon-requiring exclusion lists in
# .github/workflows/ci.yml: it is the half of this ADR's evidence that every CI leg can actually run.
agentengine_add_test(test_ustar_writer LIBS agentengine::core agentengine_warnings)

# ---- ADR-102 Phase 4 -- MandatorySandboxProvider<Surface>/RunCommandTool
# (sandbox/mandatory_sandbox_provider.hpp), composed as a REAL agentengine::rt::AgentSession's actual
# HistoryProviderT, driven through the real, unmodified invoke_tool() 10-step pipeline for the first
# time in this design's history. Needs agentengine::worktree_store the same way test_ledger/
# test_sandbox_runtime do (real Ledger/compute_digest/open_within_mount_root).
# REQUIRES a running Docker daemon reachable via `docker` on PATH.
agentengine_add_test(test_mandatory_sandbox_provider
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)
set_tests_properties(test_mandatory_sandbox_provider PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- ADR-114 -- MandatorySandboxProvider<Surface>'s task-branch tools (start_task_branch/
# run_in_task_branch/commit_task_branch/discard_task_branch), the A10 promotion giving
# SandboxRuntime::merge_into() its first real production caller. Same link requirements as
# test_mandatory_sandbox_provider (real Ledger/compute_digest/open_within_mount_root + Docker).
# REQUIRES a running Docker daemon reachable via `docker` on PATH.
agentengine_add_test(test_task_branch_tools
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)
set_tests_properties(test_task_branch_tools PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- ADR-114 §5/§6 residual, closed: real, two-OS-thread concurrent dispatch across all four
# task-branch verbs on ONE MandatorySandboxProvider instance -- mirrors test_rt_block_on's own real
# cross-thread contention methodology, applied to this class's own task_branch_mutex_ specifically
# rather than to the generic AsyncMutex primitive alone. REQUIRES a running Docker daemon.
agentengine_add_test(test_task_branch_concurrent_dispatch
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)
set_tests_properties(test_task_branch_concurrent_dispatch PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- ADR-126 -- closes ADR-114 §5/§6's own "active_'s table has no durability of its own" residual:
# bind_sandbox() now recovers orphaned child task branches from a durable-Store Ledger after a
# simulated crash. Docker-independent (never calls run_in_task_branch()).
agentengine_add_test(test_task_branch_durability_recovery
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)

# ---- ADR-128 -- closes ADR-126 §5's own "root-branch recovery remains the caller's own, separate,
# already-disclosed responsibility" residual: MandatorySandboxProvider::bind_root_branch() resolves
# (reclaim-if-orphaned, create-if-not) an owner's own deterministic root branch by identity alone.
# Docker-independent (never calls run_in_task_branch()).
agentengine_add_test(test_root_branch_recovery
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)

# ---- ADR-102 Phase 5 -- the real cli_chat.cpp composition pattern (one ContextProvider embedding
# MandatorySandboxProvider and merging its contribution outside any skill-scoping), against a FAKE
# ExecutionSurface -- no Docker daemon needed, this proves the DECLARATION-time composition, not
# run_command's real execution (already proven against real Docker by test_sandbox_runtime/
# test_mandatory_sandbox_provider). Closes a real gap an independent red-team pass found: this exact
# composition had zero automated coverage before this file.
agentengine_add_test(test_mandatory_sandbox_provider_composed
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)

# ---- ADR-132 -- THE definitive, full-stack proof: MandatorySandboxProvider::commit_task_branch(),
# through the REAL, unmodified production tool surface, genuinely succeeds after a simulated crash with
# real, durable content -- root-branch recovery (ADR-128), child-branch recovery (ADR-126), content
# durability (ADR-130), and the Store-genericity fix that lets them compose (ADR-132) all together.
agentengine_add_test(test_task_branch_content_durability_integration
  LIBS agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)
