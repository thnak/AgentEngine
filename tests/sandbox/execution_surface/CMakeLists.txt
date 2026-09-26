# tests/sandbox/execution_surface/: Docker and containerd execution surfaces.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "sandbox/execution_surface" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- ADR-102 Phase 5 (ComposedContextProvider<Ms...> real production consumer slice):
# SandboxToolProvider (above) and MandatorySandboxProvider<DockerExecutionSurface> (ADR-102 Phase 4)
# composed into ONE real AgentSession's actual HistoryProviderT via ComposedContextProvider<Ms...> --
# the first time ComposedContextProvider<Ms...> is driven through a real session.start_run() anywhere
# in this codebase.
#
# Portable across Windows and Linux (2026-08-29, decisions/ADR-105-sandbox-tool-provider-composed-
# linux-parity.md) -- moved OUT of the surrounding `if(WIN32)` block below, same reasoning as
# test_mediated_shell_runner_no_process_creation above: `agentengine::mediated_shell_runner`
# (SandboxToolProvider/SessionShellSandbox/MediatedFileSystemAdapter) has built on both platforms
# since ADR-103; this target's own WIN32 gate was a stale leftover from before that parity landed,
# never a real dependency of this test's own code. REQUIRES a real Docker daemon on PATH, same as
# every other MandatorySandboxProvider<DockerExecutionSurface> consumer, on either platform.
add_executable(test_composed_sandbox_providers_live test_composed_sandbox_providers_live.cpp)
target_link_libraries(test_composed_sandbox_providers_live PRIVATE
  agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
  agentengine::sandbox_io agentengine_warnings)
# ADR-175 grew the rt headers this TU pulls in (resume_home.hpp, and every driver that now posts to a
# home) enough to tip its single .obj past MSVC's COFF section-count limit (C1128) -- on the ASan leg
# only, whose instrumentation adds sections of its own; the plain MSVC and clang-cl legs still build it.
# Issue #80 then pushed the plain legs over too: this TU instantiates three whole sandbox-provider
# stacks, and the two fields `RunCommandReply`/`TaskBranchRunReply` gained cost another AE_JSON_SCHEMA
# codec instantiation per reply type per stack. A mechanical compiler limit either way, not a code
# defect: same documented fix, and the same scoped-to-one-target reasoning, as agentengine_cli_chat's
# own /bigobj in the root CMakeLists.txt.
if(MSVC)
  target_compile_options(test_composed_sandbox_providers_live PRIVATE /bigobj)
endif()
add_test(NAME test_composed_sandbox_providers_live COMMAND test_composed_sandbox_providers_live)
# RESOURCE_LOCK, not a lower -j: this and every other real-Docker-daemon test below share ONE
# `docker_daemon` lock name, so CTest never schedules two of them concurrently regardless of -j,
# while everything else in the suite keeps running at full parallelism. Added after a real CI
# failure (not a hypothetical): ubuntu-latest's `-j 4` ran this test alongside test_sandbox_runtime/
# test_docker_orphan_reap/test_mandatory_sandbox_provider against the SAME shared daemon, and
# test_docker_orphan_reap's own POSITIVE CONTROL (a container matching THIS process's live
# pid+start-key must NOT be reaped) failed under that contention -- daemon-level flakiness, not a
# logic bug (the identical code passed clean, repeatedly, in this session's own non-concurrent
# Windows/MSVC verification runs).
set_tests_properties(test_composed_sandbox_providers_live PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- ADR-106 (ContainerdExecutionSurface promotion): the SECOND real ExecutionSurface conformer,
# closing the identity-native design's own "only one conformer exists, does the three-verb shape
# generalize" residual. Linux-only (agentengine/sandbox/containerd_execution_surface.hpp's own top
# comment -- `ctr` only meaningfully talks to a local containerd Unix socket), header-only (no new
# CMake library target needed, matching DockerExecutionSurface's own shape). REQUIRES a running
# containerd daemon reachable via the `ctr` CLI on PATH -- same "test just fails cleanly if the daemon
# isn't reachable" posture as every other ExecutionSurface conformer's own test, not a special opt-in
# flag (unlike test_kata_backend_linux, which needs a materially heavier KVM/Kata deployment).
if(NOT WIN32)
  agentengine_add_test(test_containerd_execution_surface LIBS agentengine::core agentengine_warnings)
  # Same RESOURCE_LOCK reasoning as docker_daemon above, a separate lock name because this and its
  # sibling below hit a DIFFERENT shared daemon (containerd, via `ctr`) than the docker_daemon-locked
  # tests do.
  set_tests_properties(test_containerd_execution_surface PROPERTIES TIMEOUT 60 RESOURCE_LOCK containerd_daemon)

  # ---- ADR-172 (GitHub issue #66) -- the containerd sibling of ADR-171's Docker isolation work.
  # C1-C6 are offline (argv shape, --cpus formatting, the 008 §2 mapping, and TWO refusals: a
  # NetPolicy allowlist and -- unlike Docker -- a pids limit, which `ctr run` genuinely cannot
  # express). L1-L5 need a live containerd and prove what the kernel applied: only loopback, a ZEROED
  # CapBnd read from inside the container (proving the 14-name drop list is complete), the host-side
  # cgroup memory/cpu values, and that the surface threads a caller's isolation through reset().
  # L2 is the positive control. Same REQUIRES as test_containerd_execution_surface above.
  agentengine_add_test(test_containerd_isolation LIBS agentengine::core agentengine_warnings)
  set_tests_properties(test_containerd_isolation PROPERTIES TIMEOUT 120 RESOURCE_LOCK containerd_daemon)

  # ---- ADR-106 §7 residual, closed: the ContainerdExecutionSurface counterpart to
  # test_composed_sandbox_providers_live above -- proves MandatorySandboxProvider<
  # ContainerdExecutionSurface> (not just ContainerdExecutionSurface standalone) works through the
  # real, unmodified session.start_run() -> invoke_tool() pipeline, composed alongside
  # SandboxToolProvider. Same REQUIRES as test_containerd_execution_surface above.
  agentengine_add_test(test_composed_containerd_providers_live
    LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
      agentengine::sandbox_io agentengine_warnings)
  set_tests_properties(test_composed_containerd_providers_live PROPERTIES RESOURCE_LOCK containerd_daemon)
endif()

# ---- ADR-108 -- DockerCliBackend::reap_orphans(), the real orphan-container garbage-collection fix
# for the residual DockerExecutionSurface's own header comment has disclosed since ADR-104/ADR-106.
# REQUIRES a running Docker daemon reachable via `docker` on PATH, same posture as test_sandbox_runtime
# above.
agentengine_add_test(test_docker_orphan_reap LIBS agentengine::core agentengine_warnings)
set_tests_properties(test_docker_orphan_reap PROPERTIES RESOURCE_LOCK docker_daemon)

add_executable(test_execution_surface_image_identity test_execution_surface_image_identity.cpp)
target_link_libraries(test_execution_surface_image_identity PRIVATE
  agentengine::core agentengine::worktree_store agentengine::sandbox_io agentengine_warnings)
if(MSVC)
  target_compile_options(test_execution_surface_image_identity PRIVATE /bigobj)
endif()
add_test(NAME test_execution_surface_image_identity COMMAND test_execution_surface_image_identity)
set_tests_properties(test_execution_surface_image_identity PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- docker-execution-surface-argv-hardening (issue #50) -- run_argv()'s Job-Object timeout kill,
# no longer through a cmd.exe layer, still leaves no orphaned host docker.exe process. Windows-only:
# see the test file's own top comment for why (the historical bug and its fix are both Windows-specific;
# the file itself is a no-op main() on other platforms so it stays buildable everywhere for anyone who
# still lists it, but only actually gated into the suite here on Windows).
if(WIN32)
  agentengine_add_test(test_docker_run_argv_timeout LIBS agentengine::core agentengine_warnings)
  set_tests_properties(test_docker_run_argv_timeout PROPERTIES RESOURCE_LOCK docker_daemon TIMEOUT 60)
endif()

# ---- code-review finding (post-ADR-164): host_path.string()'s Windows ACP-vs-widen()'s-UTF-8
# mismatch, fixed by docker_cli_detail::path_to_utf8() -- proves the fix for real, cross-platform
# (the underlying bug was Windows-only, but copy_to_container()/copy_from_container() are shared code,
# so this runs everywhere the other Docker-backed tests do).
agentengine_add_test(test_docker_non_ascii_path LIBS agentengine::core agentengine_warnings)
set_tests_properties(test_docker_non_ascii_path PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- ADR-171 (GitHub issue #63) -- real, fail-closed container isolation for DockerCliBackend/
# DockerExecutionSurface, which previously emitted a `docker run` with NO isolation flags at all.
# I1-I5 are offline (argv shape, --cpus formatting, the 008 §2 ResourceLimits/NetPolicy mapping, and
# the security-critical refusal of a NetPolicy allowlist). L1-L4 need a live daemon and prove the
# KERNEL applied the limits, by reading the container's own /sys/class/net and cgroup files from
# inside it -- with L2 as the positive control that the probe can report the opposite.
# REQUIRES a running Docker daemon reachable via `docker` on PATH.
agentengine_add_test(test_docker_isolation LIBS agentengine::core agentengine_warnings)
set_tests_properties(test_docker_isolation PROPERTIES RESOURCE_LOCK docker_daemon)

# ---- ADR-174 (GitHub issue #68) -- the LIVE half: a container seeded through `docker cp -` owns its
# workspace as root and can WRITE what it was handed, with ADR-171's --cap-drop ALL fully in place.
# S7 additionally puts the long and ".."-containing names through the real daemon, so the PAX header
# is proven against the daemon's own tar reader and not only against our own bytes.
# REQUIRES a running Docker daemon reachable via `docker` on PATH, same posture as
# test_docker_isolation/test_sandbox_runtime.
agentengine_add_test(test_docker_seed_ownership LIBS agentengine::core agentengine_warnings)
set_tests_properties(test_docker_seed_ownership PROPERTIES TIMEOUT 180 RESOURCE_LOCK docker_daemon)
