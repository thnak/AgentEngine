# tests/protocol/anthropic/: the Anthropic ChatClient.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "protocol/anthropic" ctest label for every test here (cmake/AgentEngineTests.cmake).

if(AGENTENGINE_WITH_HTTPS)
  # ---- Milestone 5 Phase E: Anthropic ChatClient backend (protocol/anthropic/chat_client.hpp) ------
  agentengine_add_test(test_anthropic_chat_client_translation
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings)

  agentengine_add_test(test_anthropic_chat_client_live
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)

  # ---- ADR-020 gates G1-G7: 004 §2's portable reasoning_effort, mapped down by both backends ------
  # Drives BOTH backends from the same enumerator values in one file, on purpose: the amendment's
  # claim is portability across two structurally different native shapes (a flat string vs a computed
  # token budget), and a per-backend suite could only ever prove half of it. Carries the two negative
  # controls the design turns on -- the capability gate refusing a level, and Anthropic's own vendor
  # constraints being enforced client-side because a gateway was measured NOT enforcing them.
  # Pure request building, no network, so it is a default-suite test.
  agentengine_add_test(test_reasoning_effort_portability
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()
