# tests/backends/kata/: the Kata Containers backend (src/backends/kata/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "backends/kata" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- KataBackend end-to-end test (src/backends/kata/) -- Linux only, requires a real Kata/-------
# containerd deployment (kata_backend.hpp's own header comment has the exact precondition). Gated
# behind its own flag, separate from AGENTENGINE_LINUX_SANDBOX_TESTS, since it needs a materially
# heavier deployment (containerd + a pinned Kata Containers release + KVM) than that flag's own
# namespace/cgroup-only precondition.
if(NOT WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND)
  option(AGENTENGINE_KATA_SANDBOX_TESTS
    "Build/run the KataBackend end-to-end test (needs a running containerd + Kata Containers deployment with a kata-clh runtime class already configured)"
    OFF)
  # Host-free: the egress policy generators/parsers and HostEgress's fail-closed ordering through a scripted
  # runner (ADR-240). Needs no VM, root or nft, so it is not behind the sandbox gate.
  agentengine_add_test(test_kata_egress_policy_render
    LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
  if(AGENTENGINE_KATA_SANDBOX_TESTS)
    agentengine_add_test(test_kata_backend_linux
      LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
    # Real VM boot + several `ctr` round trips per case -- generous but bounded, matching this
    # suite's own Machine Safety discipline (CLAUDE.md) rather than an unbounded wait.
    set_tests_properties(test_kata_backend_linux PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

    # ---- Slice 2 (MountSpec/ResourceLimits/NetPolicy enforcement) end-to-end test ------------------
    agentengine_add_test(test_kata_backend_slice2_linux
      LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
    # Several real VM boots (one per case, deliberately not reused across cases so each is an
    # independent create()) plus a deliberate 10s guest sleep under a 3s wall_ms cap -- generous but
    # bounded.
    set_tests_properties(test_kata_backend_slice2_linux PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

    # ---- Slice 4 (008 §9 G1/G2 abuse-corpus promotion-gate evidence) end-to-end test ---------------
    agentengine_add_test(test_kata_backend_abuse_corpus_linux
      LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
    # Several real VM boots plus a deliberate positive-control wall_ms=3000 case -- generous but
    # bounded, same posture as this suite's other Kata tests.
    set_tests_properties(test_kata_backend_abuse_corpus_linux PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

    # ---- Slice 9/10 (--config-mode OCI-spec pipeline + real NetPolicy allowlist via manual netns +
    # cnitool + nftables, decisions/archive/ADR-093-kata-backend-netpolicy-allowlist-config-cni.md) end-to-end
    # test. Cases 3-5 need `cnitool`/a CNI plugin+config/`nft` on the host in addition to this block's
    # own containerd+Kata precondition -- see kata_backend.hpp's own header comment.
    agentengine_add_test(test_kata_backend_slice9_10_linux
      LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
    # Real VM boots plus real netns/CNI/nftables setup and two live HTTPS exec() round trips
    # (allowed + blocked) -- generous but bounded, same posture as this suite's other Kata tests.
    set_tests_properties(test_kata_backend_slice9_10_linux PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

    # ---- Egress policy acceptance (ADR-240, GitHub #194): allowlist + L2/L3 containment + net_bytes, each with a
    # positive control. Needs root, python3, the python:3.12-alpine image pulled and operator FORWARD accepts for
    # aekata0 <-> aepvw0 (comment ae-prove-temp) -- see tests/backends/kata/egress_test_support.hpp.
    agentengine_add_test(test_kata_backend_egress_linux
      LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
    target_include_directories(test_kata_backend_egress_linux PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(test_kata_backend_egress_linux PROPERTIES TIMEOUT 1500 RUN_SERIAL TRUE)

    # Lifecycle and fail-closed behaviour (table loss, tampering, teardown, sweep, parallelism, injected create
    # failures, idempotent install, A-record cap). Same preconditions as above; also edits /etc/hosts temporarily.
    agentengine_add_test(test_kata_backend_egress_lifecycle_linux
      LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
    target_include_directories(test_kata_backend_egress_lifecycle_linux PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(test_kata_backend_egress_lifecycle_linux PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)

    # Scaling of the shared table with N bridge ports (netns+veth lab guests, not microVMs) plus the per-sandbox-table
    # layout as the positive control. Needs FORWARD accepts for aepvbr0 <-> aepvw0 (comment ae-prove-temp).
    agentengine_add_test(test_kata_egress_scale_linux
      LIBS agentengine::core agentengine::kata_backend agentengine_warnings)
    target_include_directories(test_kata_egress_scale_linux PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(test_kata_egress_scale_linux PROPERTIES TIMEOUT 1500 RUN_SERIAL TRUE)
  endif()
endif()
