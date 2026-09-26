# tests/protocol/openai/: the OpenAI-compatible ChatClient and embedder, and live runs against OpenAI-compatible endpoints.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "protocol/openai" ctest label for every test here (cmake/AgentEngineTests.cmake).

if(AGENTENGINE_WITH_HTTPS)
  # ---- Milestone 5 Phase D: OpenAI-compatible ChatClient backend (protocol/openai/chat_client.hpp) --
  # Translation logic (D1 request/response, D3 tool-schema shaping, D4 structured-output shaping, D2
  # SSE/chunked-transfer parsing) proven offline against literal wire-format JSON -- no TLS/mbedTLS
  # dependency needed for this target, only the header + core.
  agentengine_add_test(test_openai_chat_client_translation
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings)

  # End-to-end proof against a real local TLS server: secret resolution -> request build -> real
  # HTTPS exchange -> response/stream parse, for both chat() and chat_stream().
  agentengine_add_test(test_openai_chat_client_live
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)

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
  agentengine_add_test(test_openrouter_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- ADR-063 §2.5: OpenAIEmbedder (protocol/openai/embedder.hpp) -- offline-provable surface --
  # request/response construction and embed_batch()'s two synchronous pre-network gates. No server,
  # runs unconditionally, mirrors test_openai_chat_client_translation.cpp's own offline split.
  agentengine_add_test(test_openai_embedder
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)

  # ---- ADR-063 §3 claim 4's own disproof test: OpenAIEmbedder against REAL api.openrouter.ai -----
  agentengine_add_test(test_openai_embedder_openrouter_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_openai_embedder_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- OpenAIChatClient against REAL OpenRouter, with a CONCRETELY NAMED OpenAI model (not the
  # routing alias test_openrouter_live_e2e.cpp uses) -- see this file's own top comment for why that
  # distinction is the one new claim it adds.
  agentengine_add_test(test_openai_chat_client_openrouter_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_openai_chat_client_openrouter_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- Issue #49's own follow-up comment: drives OpenAIChatClient::chat_stream() directly against a
  # real OpenRouter reasoning model and through rt::detail::drain_streaming_response() itself, proving
  # both halves of the fix (the new ModelReasoningDelta forwarding, and OpenAIChatClient's own new
  # reasoning/reasoning_content parsing) end to end -- see this file's own top comment.
  agentengine_add_test(test_reasoning_model_delta_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_reasoning_model_delta_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  agentengine_add_test(test_llamacpp_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_llamacpp_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ---- 004 §3's OpenAI-compatible backend against a REAL, LOCAL Ollama server -- the other local
  # server that section's table names by name, alongside llama.cpp above.
  agentengine_add_test(test_ollama_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_ollama_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)
endif()
